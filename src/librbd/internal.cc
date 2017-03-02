// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "include/int_types.h"

#include <errno.h>
#include <limits.h>

#include "include/types.h"
#include "include/uuid.h"
#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Throttle.h"
#include "common/event_socket.h"
#include "cls/lock/cls_lock_client.h"
#include "include/stringify.h"

#include "cls/rbd/cls_rbd.h"
#include "cls/rbd/cls_rbd_types.h"
#include "cls/rbd/cls_rbd_client.h"
#include "cls/journal/cls_journal_types.h"
#include "cls/journal/cls_journal_client.h"

#include "librbd/AioCompletion.h"
#include "librbd/AioImageRequest.h"
#include "librbd/AioImageRequestWQ.h"
#include "librbd/AioObjectRequest.h"
#include "librbd/image/CreateRequest.h"
#include "librbd/DiffIterate.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/internal.h"
#include "librbd/Journal.h"
#include "librbd/journal/Types.h"
#include "librbd/mirror/DisableRequest.h"
#include "librbd/mirror/EnableRequest.h"
#include "librbd/MirroringWatcher.h"
#include "librbd/ObjectMap.h"
#include "librbd/Operations.h"
#include "librbd/parent_types.h"
#include "librbd/Utils.h"
#include "librbd/exclusive_lock/AutomaticPolicy.h"
#include "librbd/exclusive_lock/BreakRequest.h"
#include "librbd/exclusive_lock/GetLockerRequest.h"
#include "librbd/exclusive_lock/StandardPolicy.h"
#include "librbd/exclusive_lock/Types.h"
#include "librbd/operation/TrimRequest.h"

#include "journal/Journaler.h"

#include <boost/scope_exit.hpp>
#include <boost/variant.hpp>
#include "include/assert.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd: "

#define rbd_howmany(x, y)  (((x) + (y) - 1) / (y))

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;
// list binds to list() here, so std::list is explicitly used below

using ceph::bufferlist;
using librados::snap_t;
using librados::IoCtx;
using librados::Rados;

namespace librbd {

namespace {

int validate_pool(IoCtx &io_ctx, CephContext *cct) {
  if (!cct->_conf->rbd_validate_pool) {
    return 0;
  }

  int r = io_ctx.stat(RBD_DIRECTORY, NULL, NULL);
  if (r == 0) {
    return 0;
  } else if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to stat RBD directory: " << cpp_strerror(r) << dendl;
    return r;
  }

  // allocate a self-managed snapshot id if this a new pool to force
  // self-managed snapshot mode
  uint64_t snap_id;
  r = io_ctx.selfmanaged_snap_create(&snap_id);
  if (r == -EINVAL) {
    lderr(cct) << "pool not configured for self-managed RBD snapshot support"
               << dendl;
    return r;
  } else if (r < 0) {
    lderr(cct) << "failed to allocate self-managed snapshot: "
               << cpp_strerror(r) << dendl;
    return r;
  }

  r = io_ctx.selfmanaged_snap_remove(snap_id);
  if (r < 0) {
    lderr(cct) << "failed to release self-managed snapshot " << snap_id
               << ": " << cpp_strerror(r) << dendl;
  }
  return 0;
}

int validate_mirroring_enabled(ImageCtx *ictx) {
  CephContext *cct = ictx->cct;
  cls::rbd::MirrorImage mirror_image_internal;
  int r = cls_client::mirror_image_get(&ictx->md_ctx, ictx->id,
      &mirror_image_internal);
  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to retrieve mirroring state: " << cpp_strerror(r)
      << dendl;
    return r;
  } else if (mirror_image_internal.state !=
               cls::rbd::MIRROR_IMAGE_STATE_ENABLED) {
    lderr(cct) << "mirroring is not currently enabled" << dendl;
    return -EINVAL;
  }
  return 0;
}

int mirror_image_enable_internal(ImageCtx *ictx) {
  CephContext *cct = ictx->cct;
  C_SaferCond cond;

  if ((ictx->features & RBD_FEATURE_JOURNALING) == 0) {
    lderr(cct) << "cannot enable mirroring: journaling is not enabled" << dendl;
    return -EINVAL;
  }

  mirror::EnableRequest<ImageCtx> *req =
    mirror::EnableRequest<ImageCtx>::create(ictx, &cond);
  req->send();

  int r = cond.wait();
  if (r < 0) {
    lderr(cct) << "cannot enable mirroring: " << cpp_strerror(r) << dendl;
    return r;
  }

  return 0;
}

int mirror_image_disable_internal(ImageCtx *ictx, bool force,
                                  bool remove=true) {
  CephContext *cct = ictx->cct;
  C_SaferCond cond;

  mirror::DisableRequest<ImageCtx> *req =
    mirror::DisableRequest<ImageCtx>::create(ictx, force, remove, &cond);
  req->send();

  int r = cond.wait();
  if (r < 0) {
    lderr(cct) << "cannot disable mirroring: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

} // anonymous namespace

  int detect_format(IoCtx &io_ctx, const string &name,
		    bool *old_format, uint64_t *size)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    if (old_format)
      *old_format = true;
    int r = io_ctx.stat(util::old_header_name(name), size, NULL);
    if (r == -ENOENT) {
      if (old_format)
	*old_format = false;
      r = io_ctx.stat(util::id_obj_name(name), size, NULL);
      if (r < 0)
	return r;
    } else if (r < 0) {
      return r;
    }

    ldout(cct, 20) << "detect format of " << name << " : "
		   << (old_format ? (*old_format ? "old" : "new") :
		       "don't care")  << dendl;
    return 0;
  }

  bool has_parent(int64_t parent_pool_id, uint64_t off, uint64_t overlap)
  {
    return (parent_pool_id != -1 && off <= overlap);
  }

  void init_rbd_header(struct rbd_obj_header_ondisk& ondisk,
		       uint64_t size, int order, uint64_t bid)
  {
    uint32_t hi = bid >> 32;
    uint32_t lo = bid & 0xFFFFFFFF;
    uint32_t extra = rand() % 0xFFFFFFFF;
    memset(&ondisk, 0, sizeof(ondisk));

    memcpy(&ondisk.text, RBD_HEADER_TEXT, sizeof(RBD_HEADER_TEXT));
    memcpy(&ondisk.signature, RBD_HEADER_SIGNATURE,
	   sizeof(RBD_HEADER_SIGNATURE));
    memcpy(&ondisk.version, RBD_HEADER_VERSION, sizeof(RBD_HEADER_VERSION));

    snprintf(ondisk.block_name, sizeof(ondisk.block_name), "rb.%x.%x.%x",
	     hi, lo, extra);

    ondisk.image_size = size;
    ondisk.options.order = order;
    ondisk.options.crypt_type = RBD_CRYPT_NONE;
    ondisk.options.comp_type = RBD_COMP_NONE;
    ondisk.snap_seq = 0;
    ondisk.snap_count = 0;
    ondisk.reserved = 0;
    ondisk.snap_names_len = 0;
  }

  void image_info(ImageCtx *ictx, image_info_t& info, size_t infosize)
  {
    int obj_order = ictx->order;
    ictx->snap_lock.get_read();
    info.size = ictx->get_image_size(ictx->snap_id);
    ictx->snap_lock.put_read();
    info.obj_size = 1ULL << obj_order;
    info.num_objs = Striper::get_num_objects(ictx->layout, info.size);
    info.order = obj_order;
    strncpy(info.block_name_prefix, ictx->object_prefix.c_str(),
            RBD_MAX_BLOCK_NAME_SIZE);
    info.block_name_prefix[RBD_MAX_BLOCK_NAME_SIZE - 1] = '\0';

    // clear deprecated fields
    info.parent_pool = -1L;
    info.parent_name[0] = '\0';
  }

  uint64_t oid_to_object_no(const string& oid, const string& object_prefix)
  {
    istringstream iss(oid);
    // skip object prefix and separator
    iss.ignore(object_prefix.length() + 1);
    uint64_t num;
    iss >> std::hex >> num;
    return num;
  }

  void trim_image(ImageCtx *ictx, uint64_t newsize, ProgressContext& prog_ctx)
  {
    assert(ictx->owner_lock.is_locked());
    assert(ictx->exclusive_lock == nullptr ||
	   ictx->exclusive_lock->is_lock_owner());

    C_SaferCond ctx;
    ictx->snap_lock.get_read();
    operation::TrimRequest<> *req = operation::TrimRequest<>::create(
      *ictx, &ctx, ictx->size, newsize, prog_ctx);
    ictx->snap_lock.put_read();
    req->send();

    int r = ctx.wait();
    if (r < 0) {
      lderr(ictx->cct) << "warning: failed to remove some object(s): "
		       << cpp_strerror(r) << dendl;
    }
  }

  int read_header_bl(IoCtx& io_ctx, const string& header_oid,
		     bufferlist& header, uint64_t *ver)
  {
    int r;
    uint64_t off = 0;
#define READ_SIZE 4096
    do {
      bufferlist bl;
      r = io_ctx.read(header_oid, bl, READ_SIZE, off);
      if (r < 0)
	return r;
      header.claim_append(bl);
      off += r;
    } while (r == READ_SIZE);

    if (header.length() < sizeof(RBD_HEADER_TEXT) ||
	memcmp(RBD_HEADER_TEXT, header.c_str(), sizeof(RBD_HEADER_TEXT))) {
      CephContext *cct = (CephContext *)io_ctx.cct();
      lderr(cct) << "unrecognized header format" << dendl;
      return -ENXIO;
    }

    if (ver)
      *ver = io_ctx.get_last_version();

    return 0;
  }

  int read_header(IoCtx& io_ctx, const string& header_oid,
		  struct rbd_obj_header_ondisk *header, uint64_t *ver)
  {
    bufferlist header_bl;
    int r = read_header_bl(io_ctx, header_oid, header_bl, ver);
    if (r < 0)
      return r;
    if (header_bl.length() < (int)sizeof(*header))
      return -EIO;
    memcpy(header, header_bl.c_str(), sizeof(*header));

    return 0;
  }

  int tmap_set(IoCtx& io_ctx, const string& imgname)
  {
    bufferlist cmdbl, emptybl;
    __u8 c = CEPH_OSD_TMAP_SET;
    ::encode(c, cmdbl);
    ::encode(imgname, cmdbl);
    ::encode(emptybl, cmdbl);
    return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  }

  int tmap_rm(IoCtx& io_ctx, const string& imgname)
  {
    bufferlist cmdbl;
    __u8 c = CEPH_OSD_TMAP_RM;
    ::encode(c, cmdbl);
    ::encode(imgname, cmdbl);
    return io_ctx.tmap_update(RBD_DIRECTORY, cmdbl);
  }

  typedef boost::variant<std::string,uint64_t> image_option_value_t;
  typedef std::map<int,image_option_value_t> image_options_t;
  typedef std::shared_ptr<image_options_t> image_options_ref;

  enum image_option_type_t {
    STR,
    UINT64,
  };

  const std::map<int, image_option_type_t> IMAGE_OPTIONS_TYPE_MAPPING = {
    {RBD_IMAGE_OPTION_FORMAT, UINT64},
    {RBD_IMAGE_OPTION_FEATURES, UINT64},
    {RBD_IMAGE_OPTION_ORDER, UINT64},
    {RBD_IMAGE_OPTION_STRIPE_UNIT, UINT64},
    {RBD_IMAGE_OPTION_STRIPE_COUNT, UINT64},
    {RBD_IMAGE_OPTION_JOURNAL_ORDER, UINT64},
    {RBD_IMAGE_OPTION_JOURNAL_SPLAY_WIDTH, UINT64},
    {RBD_IMAGE_OPTION_JOURNAL_POOL, STR},
    {RBD_IMAGE_OPTION_FEATURES_SET, UINT64},
    {RBD_IMAGE_OPTION_FEATURES_CLEAR, UINT64},
    {RBD_IMAGE_OPTION_DATA_POOL, STR},
  };

  std::string image_option_name(int optname) {
    switch (optname) {
    case RBD_IMAGE_OPTION_FORMAT:
      return "format";
    case RBD_IMAGE_OPTION_FEATURES:
      return "features";
    case RBD_IMAGE_OPTION_ORDER:
      return "order";
    case RBD_IMAGE_OPTION_STRIPE_UNIT:
      return "stripe_unit";
    case RBD_IMAGE_OPTION_STRIPE_COUNT:
      return "stripe_count";
    case RBD_IMAGE_OPTION_JOURNAL_ORDER:
      return "journal_order";
    case RBD_IMAGE_OPTION_JOURNAL_SPLAY_WIDTH:
      return "journal_splay_width";
    case RBD_IMAGE_OPTION_JOURNAL_POOL:
      return "journal_pool";
    case RBD_IMAGE_OPTION_FEATURES_SET:
      return "features_set";
    case RBD_IMAGE_OPTION_FEATURES_CLEAR:
      return "features_clear";
    case RBD_IMAGE_OPTION_DATA_POOL:
      return "data_pool";
    default:
      return "unknown (" + stringify(optname) + ")";
    }
  }

  std::ostream &operator<<(std::ostream &os, rbd_image_options_t &opts) {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    os << "[";

    for (image_options_t::const_iterator i = (*opts_)->begin();
	 i != (*opts_)->end(); ++i) {
      os << (i == (*opts_)->begin() ? "" : ", ") << image_option_name(i->first)
	 << "=" << i->second;
    }

    os << "]";

    return os;
  }

  std::ostream &operator<<(std::ostream &os, ImageOptions &opts) {
    os << "[";

    const char *delimiter = "";
    for (auto &i : IMAGE_OPTIONS_TYPE_MAPPING) {
      if (i.second == STR) {
	std::string val;
	if (opts.get(i.first, &val) == 0) {
	  os << delimiter << image_option_name(i.first) << "=" << val;
	  delimiter = ", ";
	}
      } else if (i.second == UINT64) {
	uint64_t val;
	if (opts.get(i.first, &val) == 0) {
	  os << delimiter << image_option_name(i.first) << "=" << val;
	  delimiter = ", ";
	}
      }
    }

    os << "]";

    return os;
  }

  void image_options_create(rbd_image_options_t* opts)
  {
    image_options_ref* opts_ = new image_options_ref(new image_options_t());

    *opts = static_cast<rbd_image_options_t>(opts_);
  }

  void image_options_create_ref(rbd_image_options_t* opts,
				rbd_image_options_t orig)
  {
    image_options_ref* orig_ = static_cast<image_options_ref*>(orig);
    image_options_ref* opts_ = new image_options_ref(*orig_);

    *opts = static_cast<rbd_image_options_t>(opts_);
  }

  void image_options_destroy(rbd_image_options_t opts)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    delete opts_;
  }

  int image_options_set(rbd_image_options_t opts, int optname,
			const std::string& optval)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    std::map<int, image_option_type_t>::const_iterator i =
      IMAGE_OPTIONS_TYPE_MAPPING.find(optname);

    if (i == IMAGE_OPTIONS_TYPE_MAPPING.end() || i->second != STR) {
      return -EINVAL;
    }

    (*opts_->get())[optname] = optval;
    return 0;
  }

  int image_options_set(rbd_image_options_t opts, int optname, uint64_t optval)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    std::map<int, image_option_type_t>::const_iterator i =
      IMAGE_OPTIONS_TYPE_MAPPING.find(optname);

    if (i == IMAGE_OPTIONS_TYPE_MAPPING.end() || i->second != UINT64) {
      return -EINVAL;
    }

    (*opts_->get())[optname] = optval;
    return 0;
  }

  int image_options_get(rbd_image_options_t opts, int optname,
			std::string* optval)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    std::map<int, image_option_type_t>::const_iterator i =
      IMAGE_OPTIONS_TYPE_MAPPING.find(optname);

    if (i == IMAGE_OPTIONS_TYPE_MAPPING.end() || i->second != STR) {
      return -EINVAL;
    }

    image_options_t::const_iterator j = (*opts_)->find(optname);

    if (j == (*opts_)->end()) {
      return -ENOENT;
    }

    *optval = boost::get<std::string>(j->second);
    return 0;
  }

  int image_options_get(rbd_image_options_t opts, int optname, uint64_t* optval)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    std::map<int, image_option_type_t>::const_iterator i =
      IMAGE_OPTIONS_TYPE_MAPPING.find(optname);

    if (i == IMAGE_OPTIONS_TYPE_MAPPING.end() || i->second != UINT64) {
      return -EINVAL;
    }

    image_options_t::const_iterator j = (*opts_)->find(optname);

    if (j == (*opts_)->end()) {
      return -ENOENT;
    }

    *optval = boost::get<uint64_t>(j->second);
    return 0;
  }

  int image_options_is_set(rbd_image_options_t opts, int optname,
                           bool* is_set)
  {
    if (IMAGE_OPTIONS_TYPE_MAPPING.find(optname) ==
          IMAGE_OPTIONS_TYPE_MAPPING.end()) {
      return -EINVAL;
    }

    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);
    *is_set = ((*opts_)->find(optname) != (*opts_)->end());
    return 0;
  }

  int image_options_unset(rbd_image_options_t opts, int optname)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    std::map<int, image_option_type_t>::const_iterator i =
      IMAGE_OPTIONS_TYPE_MAPPING.find(optname);

    if (i == IMAGE_OPTIONS_TYPE_MAPPING.end()) {
      assert((*opts_)->find(optname) == (*opts_)->end());
      return -EINVAL;
    }

    image_options_t::const_iterator j = (*opts_)->find(optname);

    if (j == (*opts_)->end()) {
      return -ENOENT;
    }

    (*opts_)->erase(j);
    return 0;
  }

  void image_options_clear(rbd_image_options_t opts)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    (*opts_)->clear();
  }

  bool image_options_is_empty(rbd_image_options_t opts)
  {
    image_options_ref* opts_ = static_cast<image_options_ref*>(opts);

    return (*opts_)->empty();
  }

  int list_images_v2(IoCtx& io_ctx, map<string, string> &images) {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "list_images_v2 " << &io_ctx << dendl;

    // new format images are accessed by class methods
    int r;
    int max_read = 1024;
    string last_read = "";
    do {
      map<string, string> images_page;
      r = cls_client::dir_list(&io_ctx, RBD_DIRECTORY,
			   last_read, max_read, &images_page);
      if (r < 0 && r != -ENOENT) {
        lderr(cct) << "error listing image in directory: "
                   << cpp_strerror(r) << dendl;
        return r;
      } else if (r == -ENOENT) {
        break;
      }
      for (map<string, string>::const_iterator it = images_page.begin();
	   it != images_page.end(); ++it) {
	images.insert(*it);
      }
      if (!images_page.empty()) {
	last_read = images_page.rbegin()->first;
      }
      r = images_page.size();
    } while (r == max_read);

    return 0;
  }

  int list(IoCtx& io_ctx, vector<string>& names)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "list " << &io_ctx << dendl;

    bufferlist bl;
    int r = io_ctx.read(RBD_DIRECTORY, bl, 0, 0);
    if (r < 0)
      return r;

    // old format images are in a tmap
    if (bl.length()) {
      bufferlist::iterator p = bl.begin();
      bufferlist header;
      map<string,bufferlist> m;
      ::decode(header, p);
      ::decode(m, p);
      for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); ++q) {
	names.push_back(q->first);
      }
    }

    map<string, string> images;
    r = list_images_v2(io_ctx, images);
    if (r < 0) {
      lderr(cct) << "error listing v2 images: " << cpp_strerror(r) << dendl;
      return r;
    }
    for (const auto& img_pair : images) {
      names.push_back(img_pair.first);
    }

    return 0;
  }

  int flatten_children(ImageCtx *ictx, const char* snap_name, ProgressContext& pctx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "children flatten " << ictx->name << dendl;

    RWLock::RLocker l(ictx->snap_lock);
    snap_t snap_id = ictx->get_snap_id(snap_name);
    parent_spec parent_spec(ictx->md_ctx.get_id(), ictx->id, snap_id);
    map< pair<int64_t, string>, set<string> > image_info;

    int r = list_children_info(ictx, parent_spec, image_info);
    if (r < 0) {
      return r;
    }

    size_t size = image_info.size();
    if (size == 0)
      return 0;

    size_t i = 0;
    Rados rados(ictx->md_ctx);
    for ( auto &info : image_info){
      string pool = info.first.second;
      IoCtx ioctx;
      r = rados.ioctx_create2(info.first.first, ioctx);
      if (r < 0) {
        lderr(cct) << "Error accessing child image pool " << pool
                   << dendl;
        return r;
      }

      for (auto &id_it : info.second) {
	ImageCtx *imctx = new ImageCtx("", id_it, NULL, ioctx, false);
	int r = imctx->state->open(false);
	if (r < 0) {
	  lderr(cct) << "error opening image: "
		     << cpp_strerror(r) << dendl;
          delete imctx;
	  return r;
	}
	librbd::NoOpProgressContext prog_ctx;
	r = imctx->operations->flatten(prog_ctx);
	if (r < 0) {
	  lderr(cct) << "error flattening image: " << pool << "/" << id_it
		     << cpp_strerror(r) << dendl;
          imctx->state->close();
	  return r;
	}

	if ((imctx->features & RBD_FEATURE_DEEP_FLATTEN) == 0 &&
	    !imctx->snaps.empty()) {
	  imctx->parent_lock.get_read();
	  parent_info parent_info = imctx->parent_md;
	  imctx->parent_lock.put_read();

	  r = cls_client::remove_child(&imctx->md_ctx, RBD_CHILDREN,
				       parent_info.spec, imctx->id);
	  if (r < 0 && r != -ENOENT) {
	    lderr(cct) << "error removing child from children list" << dendl;
	    imctx->state->close();
	    return r;
	  }
	}
	imctx->state->close();
      }
      pctx.update_progress(++i, size);
      assert(i <= size);
    }

    return 0;
  }

  int list_children(ImageCtx *ictx, set<pair<string, string> >& names)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "children list " << ictx->name << dendl;

    RWLock::RLocker l(ictx->snap_lock);
    parent_spec parent_spec(ictx->md_ctx.get_id(), ictx->id, ictx->snap_id);
    map< pair<int64_t, string>, set<string> > image_info;

    int r = list_children_info(ictx, parent_spec, image_info);
    if (r < 0) {
      return r;
    }

    Rados rados(ictx->md_ctx);
    for ( auto &info : image_info){
      IoCtx ioctx;
      r = rados.ioctx_create2(info.first.first, ioctx);
      if (r < 0) {
        lderr(cct) << "Error accessing child image pool " << info.first.second
                   << dendl;
        return r;
      }

      for (auto &id_it : info.second) {
	string name;
	r = cls_client::dir_get_name(&ioctx, RBD_DIRECTORY, id_it, &name);
	if (r < 0) {
	  lderr(cct) << "Error looking up name for image id " << id_it
		     << " in pool " << info.first.second << dendl;
	  return r;
	}
	names.insert(make_pair(info.first.second, name));
      }
    }
    
    return 0;
  }

  int list_children_info(ImageCtx *ictx, librbd::parent_spec parent_spec,
                   map< pair<int64_t, string >, set<string> >& image_info)
  {
    CephContext *cct = ictx->cct;
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    // no children for non-layered or old format image
    if (!ictx->test_features(RBD_FEATURE_LAYERING, ictx->snap_lock))
      return 0;

    image_info.clear();
    // search all pools for children depending on this snapshot
    Rados rados(ictx->md_ctx);
    std::list<std::pair<int64_t, string> > pools;
    r = rados.pool_list2(pools);
    if (r < 0) {
      lderr(cct) << "error listing pools: " << cpp_strerror(r) << dendl; 
      return r;
    }

    for (std::list<std::pair<int64_t, string> >::const_iterator it =
         pools.begin(); it != pools.end(); ++it) {
      int64_t base_tier;
      r = rados.pool_get_base_tier(it->first, &base_tier);
      if (r == -ENOENT) {
        ldout(cct, 1) << "pool " << it->second << " no longer exists" << dendl;
        continue;
      } else if (r < 0) {
        lderr(cct) << "Error retrieving base tier for pool " << it->second
                   << dendl;
	return r;
      }
      if (it->first != base_tier) {
	// pool is a cache; skip it
	continue;
      }

      IoCtx ioctx;
      r = rados.ioctx_create2(it->first, ioctx);
      if (r == -ENOENT) {
        ldout(cct, 1) << "pool " << it->second << " no longer exists" << dendl;
        continue;
      } else if (r < 0) {
        lderr(cct) << "Error accessing child image pool " << it->second
                   << dendl;
        return r;
      }

      set<string> image_ids;
      r = cls_client::get_children(&ioctx, RBD_CHILDREN, parent_spec,
                                   image_ids);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "Error reading list of children from pool " << it->second
		   << dendl;
	return r;
      }
      image_info.insert(make_pair(make_pair(it->first, it->second), image_ids));
    }

    return 0;
  }

  int get_snap_namespace(ImageCtx *ictx,
			 const char *snap_name,
			 cls::rbd::SnapshotNamespace *snap_namespace) {
    ldout(ictx->cct, 20) << "get_snap_namespace " << ictx << " " << snap_name
			 << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    snap_t snap_id = ictx->get_snap_id(snap_name);
    if (snap_id == CEPH_NOSNAP)
      return -ENOENT;
    r = ictx->get_snap_namespace(snap_id, snap_namespace);
    return r;
  }

  int snap_is_protected(ImageCtx *ictx, const char *snap_name,
			bool *is_protected)
  {
    ldout(ictx->cct, 20) << "snap_is_protected " << ictx << " " << snap_name
			 << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    snap_t snap_id = ictx->get_snap_id(snap_name);
    if (snap_id == CEPH_NOSNAP)
      return -ENOENT;
    bool is_unprotected;
    r = ictx->is_snap_unprotected(snap_id, &is_unprotected);
    // consider both PROTECTED or UNPROTECTING to be 'protected',
    // since in either state they can't be deleted
    *is_protected = !is_unprotected;
    return r;
  }

  int create_v1(IoCtx& io_ctx, const char *imgname, uint64_t size, int order)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();

    ldout(cct, 20) << __func__ << " "  << &io_ctx << " name = " << imgname
		   << " size = " << size << " order = " << order << dendl;
    int r = validate_pool(io_ctx, cct);
    if (r < 0) {
      return r;
    }

    ldout(cct, 2) << "adding rbd image to directory..." << dendl;
    r = tmap_set(io_ctx, imgname);
    if (r < 0) {
      lderr(cct) << "error adding image to directory: " << cpp_strerror(r)
		 << dendl;
      return r;
    }

    Rados rados(io_ctx);
    uint64_t bid = rados.get_instance_id();

    ldout(cct, 2) << "creating rbd image..." << dendl;
    struct rbd_obj_header_ondisk header;
    init_rbd_header(header, size, order, bid);

    bufferlist bl;
    bl.append((const char *)&header, sizeof(header));

    string header_oid = util::old_header_name(imgname);
    r = io_ctx.write(header_oid, bl, bl.length(), 0);
    if (r < 0) {
      lderr(cct) << "Error writing image header: " << cpp_strerror(r)
		 << dendl;
      int remove_r = tmap_rm(io_ctx, imgname);
      if (remove_r < 0) {
	lderr(cct) << "Could not remove image from directory after "
		   << "header creation failed: "
		   << cpp_strerror(remove_r) << dendl;
      }
      return r;
    }

    ldout(cct, 2) << "done." << dendl;
    return 0;
  }

  int create(librados::IoCtx& io_ctx, const char *imgname, uint64_t size,
	     int *order)
  {
    uint64_t order_ = *order;
    ImageOptions opts;

    int r = opts.set(RBD_IMAGE_OPTION_ORDER, order_);
    assert(r == 0);

    r = create(io_ctx, imgname, size, opts, "", "", false);

    int r1 = opts.get(RBD_IMAGE_OPTION_ORDER, &order_);
    assert(r1 == 0);
    *order = order_;

    return r;
  }

  int create(IoCtx& io_ctx, const char *imgname, uint64_t size,
	     bool old_format, uint64_t features, int *order,
	     uint64_t stripe_unit, uint64_t stripe_count)
  {
    if (!order)
      return -EINVAL;

    uint64_t order_ = *order;
    uint64_t format = old_format ? 1 : 2;
    ImageOptions opts;
    int r;

    r = opts.set(RBD_IMAGE_OPTION_FORMAT, format);
    assert(r == 0);
    r = opts.set(RBD_IMAGE_OPTION_FEATURES, features);
    assert(r == 0);
    r = opts.set(RBD_IMAGE_OPTION_ORDER, order_);
    assert(r == 0);
    r = opts.set(RBD_IMAGE_OPTION_STRIPE_UNIT, stripe_unit);
    assert(r == 0);
    r = opts.set(RBD_IMAGE_OPTION_STRIPE_COUNT, stripe_count);
    assert(r == 0);

    r = create(io_ctx, imgname, size, opts, "", "", false);

    int r1 = opts.get(RBD_IMAGE_OPTION_ORDER, &order_);
    assert(r1 == 0);
    *order = order_;

    return r;
  }

  int create(IoCtx& io_ctx, const char *imgname, uint64_t size,
	     ImageOptions& opts,
             const std::string &non_primary_global_image_id,
             const std::string &primary_mirror_uuid,
             bool skip_mirror_enable)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 10) << __func__ << " name=" << imgname << ", "
                   << "size=" << size << ", opts=" << opts << dendl;

    uint64_t format;
    if (opts.get(RBD_IMAGE_OPTION_FORMAT, &format) != 0)
      format = cct->_conf->rbd_default_format;
    bool old_format = format == 1;


    // make sure it doesn't already exist, in either format
    int r = detect_format(io_ctx, imgname, NULL, NULL);
    if (r != -ENOENT) {
      if (r) {
	lderr(cct) << "Could not tell if " << imgname << " already exists" << dendl;
	return r;
      }
      lderr(cct) << "rbd image " << imgname << " already exists" << dendl;
      return -EEXIST;
    }

    uint64_t order = 0;
    if (opts.get(RBD_IMAGE_OPTION_ORDER, &order) != 0 || order == 0) {
      order = cct->_conf->rbd_default_order;
    }
    r = image::CreateRequest<>::validate_order(cct, order);
    if (r < 0) {
      return r;
    }

    if (old_format) {
      r = create_v1(io_ctx, imgname, size, order);
    } else {
      C_SaferCond cond;
      ContextWQ op_work_queue("librbd::op_work_queue",
                              cct->_conf->rbd_op_thread_timeout,
                              ImageCtx::get_thread_pool_instance(cct));

      std::string id = util::generate_image_id(io_ctx);
      image::CreateRequest<> *req = image::CreateRequest<>::create(
        io_ctx, imgname, id, size, opts, non_primary_global_image_id,
        primary_mirror_uuid, skip_mirror_enable, &op_work_queue, &cond);
      req->send();

      r = cond.wait();
      op_work_queue.drain();
    }

    int r1 = opts.set(RBD_IMAGE_OPTION_ORDER, order);
    assert(r1 == 0);

    return r;
  }

  /*
   * Parent may be in different pool, hence different IoCtx
   */
  int clone(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
	    IoCtx& c_ioctx, const char *c_name,
	    uint64_t features, int *c_order,
	    uint64_t stripe_unit, int stripe_count)
  {
    uint64_t order = *c_order;

    ImageOptions opts;
    opts.set(RBD_IMAGE_OPTION_FEATURES, features);
    opts.set(RBD_IMAGE_OPTION_ORDER, order);
    opts.set(RBD_IMAGE_OPTION_STRIPE_UNIT, stripe_unit);
    opts.set(RBD_IMAGE_OPTION_STRIPE_COUNT, stripe_count);

    int r = clone(p_ioctx, p_name, p_snap_name, c_ioctx, c_name, opts);
    opts.get(RBD_IMAGE_OPTION_ORDER, &order);
    *c_order = order;
    return r;
  }

  int clone(IoCtx& p_ioctx, const char *p_name, const char *p_snap_name,
	    IoCtx& c_ioctx, const char *c_name, ImageOptions& c_opts)
  {
    CephContext *cct = (CephContext *)p_ioctx.cct();
    if (p_snap_name == NULL) {
      lderr(cct) << "image to be cloned must be a snapshot" << dendl;
      return -EINVAL;
    }

    // make sure parent snapshot exists
    ImageCtx *p_imctx = new ImageCtx(p_name, "", p_snap_name, p_ioctx, true);
    int r = p_imctx->state->open(false);
    if (r < 0) {
      lderr(cct) << "error opening parent image: "
		 << cpp_strerror(r) << dendl;
      delete p_imctx;
      return r;
    }

    r = clone(p_imctx, c_ioctx, c_name, c_opts, "", "");

    int close_r = p_imctx->state->close();
    if (r == 0 && close_r < 0) {
      r = close_r;
    }

    if (r < 0) {
      return r;
    }
    return 0;
  }

  int clone(ImageCtx *p_imctx, IoCtx& c_ioctx, const char *c_name,
            ImageOptions& c_opts,
            const std::string &non_primary_global_image_id,
            const std::string &primary_mirror_uuid)
  {
    CephContext *cct = p_imctx->cct;
    if (p_imctx->snap_id == CEPH_NOSNAP) {
      lderr(cct) << "image to be cloned must be a snapshot" << dendl;
      return -EINVAL;
    }

    ldout(cct, 20) << "clone " << &p_imctx->md_ctx << " name " << p_imctx->name
                   << " snap " << p_imctx->snap_name << " to child " << &c_ioctx
                   << " name " << c_name << " opts = " << c_opts << dendl;

    bool default_format_set;
    c_opts.is_set(RBD_IMAGE_OPTION_FORMAT, &default_format_set);
    if (!default_format_set) {
      c_opts.set(RBD_IMAGE_OPTION_FORMAT, static_cast<uint64_t>(2));
    }

    uint64_t format = 0;
    c_opts.get(RBD_IMAGE_OPTION_FORMAT, &format);
    if (format < 2) {
      lderr(cct) << "format 2 or later required for clone" << dendl;
      return -EINVAL;
    }

    bool use_p_features = true;
    uint64_t features;
    if (c_opts.get(RBD_IMAGE_OPTION_FEATURES, &features) == 0) {
      if (features & ~RBD_FEATURES_ALL) {
	lderr(cct) << "librbd does not support requested features" << dendl;
	return -ENOSYS;
      }
      use_p_features = false;
    }

    // make sure child doesn't already exist, in either format
    int r = detect_format(c_ioctx, c_name, NULL, NULL);
    if (r != -ENOENT) {
      lderr(cct) << "rbd image " << c_name << " already exists" << dendl;
      return -EEXIST;
    }

    bool snap_protected;

    uint64_t order;
    uint64_t size;
    uint64_t p_features;
    int partial_r;
    librbd::NoOpProgressContext no_op;
    ImageCtx *c_imctx = NULL;
    map<string, bufferlist> pairs;
    parent_spec pspec(p_imctx->md_ctx.get_id(), p_imctx->id, p_imctx->snap_id);

    if (p_imctx->old_format) {
      lderr(cct) << "parent image must be in new format" << dendl;
      return -EINVAL;
    }

    p_imctx->snap_lock.get_read();
    p_features = p_imctx->features;
    size = p_imctx->get_image_size(p_imctx->snap_id);
    r = p_imctx->is_snap_protected(p_imctx->snap_id, &snap_protected);
    p_imctx->snap_lock.put_read();

    if ((p_features & RBD_FEATURE_LAYERING) != RBD_FEATURE_LAYERING) {
      lderr(cct) << "parent image must support layering" << dendl;
      return -ENOSYS;
    }

    if (r < 0) {
      // we lost the race with snap removal?
      lderr(cct) << "unable to locate parent's snapshot" << dendl;
      return r;
    }

    if (!snap_protected) {
      lderr(cct) << "parent snapshot must be protected" << dendl;
      return -EINVAL;
    }

    if ((p_features & RBD_FEATURE_JOURNALING) != 0) {
      bool force_non_primary = !non_primary_global_image_id.empty();
      bool is_primary;
      int r = Journal<>::is_tag_owner(p_imctx, &is_primary);
      if (r < 0) {
	lderr(cct) << "failed to determine tag ownership: " << cpp_strerror(r)
		   << dendl;
	return r;
      }
      if (!is_primary && !force_non_primary) {
	lderr(cct) << "parent is non-primary mirrored image" << dendl;
	return -EINVAL;
      }
    }

    if (use_p_features) {
      features = p_features;
    }

    order = p_imctx->order;
    if (c_opts.get(RBD_IMAGE_OPTION_ORDER, &order) != 0) {
      c_opts.set(RBD_IMAGE_OPTION_ORDER, order);
    }

    if ((features & RBD_FEATURE_LAYERING) != RBD_FEATURE_LAYERING) {
      lderr(cct) << "cloning image must support layering" << dendl;
      return -ENOSYS;
    }

    c_opts.set(RBD_IMAGE_OPTION_FEATURES, features);
    r = create(c_ioctx, c_name, size, c_opts, non_primary_global_image_id,
               primary_mirror_uuid, true);
    if (r < 0) {
      lderr(cct) << "error creating child: " << cpp_strerror(r) << dendl;
      return r;
    }

    c_imctx = new ImageCtx(c_name, "", NULL, c_ioctx, false);
    r = c_imctx->state->open(false);
    if (r < 0) {
      lderr(cct) << "Error opening new image: " << cpp_strerror(r) << dendl;
      delete c_imctx;
      goto err_remove;
    }

    r = cls_client::set_parent(&c_ioctx, c_imctx->header_oid, pspec, size);
    if (r < 0) {
      lderr(cct) << "couldn't set parent: " << cpp_strerror(r) << dendl;
      goto err_close_child;
    }

    r = cls_client::add_child(&c_ioctx, RBD_CHILDREN, pspec, c_imctx->id);
    if (r < 0) {
      lderr(cct) << "couldn't add child: " << cpp_strerror(r) << dendl;
      goto err_close_child;
    }

    r = p_imctx->state->refresh();
    if (r == 0) {
      p_imctx->snap_lock.get_read();
      r = p_imctx->is_snap_protected(p_imctx->snap_id, &snap_protected);
      p_imctx->snap_lock.put_read();
    }
    if (r < 0 || !snap_protected) {
      // we lost the race with unprotect
      r = -EINVAL;
      goto err_remove_child;
    }

    r = cls_client::metadata_list(&p_imctx->md_ctx, p_imctx->header_oid, "", 0,
                                  &pairs);
    if (r < 0 && r != -EOPNOTSUPP && r != -EIO) {
      lderr(cct) << "couldn't list metadata: " << cpp_strerror(r) << dendl;
      goto err_remove_child;
    } else if (r == 0 && !pairs.empty()) {
      r = cls_client::metadata_set(&c_ioctx, c_imctx->header_oid, pairs);
      if (r < 0) {
        lderr(cct) << "couldn't set metadata: " << cpp_strerror(r) << dendl;
        goto err_remove_child;
      }
    }

    if (c_imctx->test_features(RBD_FEATURE_JOURNALING)) {
      cls::rbd::MirrorMode mirror_mode_internal =
        cls::rbd::MIRROR_MODE_DISABLED;
      r = cls_client::mirror_mode_get(&c_imctx->md_ctx, &mirror_mode_internal);
      if (r < 0 && r != -ENOENT) {
        lderr(cct) << "failed to retrieve mirror mode: " << cpp_strerror(r)
                   << dendl;
        goto err_remove_child;
      }

      // enable mirroring now that clone has been fully created
      if (mirror_mode_internal == cls::rbd::MIRROR_MODE_POOL ||
          !non_primary_global_image_id.empty()) {
        C_SaferCond ctx;
        mirror::EnableRequest<ImageCtx> *req =
          mirror::EnableRequest<ImageCtx>::create(c_imctx->md_ctx, c_imctx->id,
                                                  non_primary_global_image_id,
                                                  c_imctx->op_work_queue, &ctx);
        req->send();

        r = ctx.wait();
        if (r < 0) {
          lderr(cct) << "failed to enable mirroring: " << cpp_strerror(r)
                     << dendl;
          goto err_remove_child;
        }
      }
    }

    ldout(cct, 2) << "done." << dendl;
    r = c_imctx->state->close();
    return r;

  err_remove_child:
    partial_r = cls_client::remove_child(&c_ioctx, RBD_CHILDREN, pspec,
                                         c_imctx->id);
    if (partial_r < 0) {
     lderr(cct) << "Error removing failed clone from list of children: "
                << cpp_strerror(partial_r) << dendl;
    }
  err_close_child:
    c_imctx->state->close();
  err_remove:
    partial_r = remove(c_ioctx, c_name, "", no_op);
    if (partial_r < 0) {
      lderr(cct) << "Error removing failed clone: "
		 << cpp_strerror(partial_r) << dendl;
    }
    return r;
  }

  int rename(IoCtx& io_ctx, const char *srcname, const char *dstname)
  {
    CephContext *cct = (CephContext *)io_ctx.cct();
    ldout(cct, 20) << "rename " << &io_ctx << " " << srcname << " -> "
		   << dstname << dendl;

    ImageCtx *ictx = new ImageCtx(srcname, "", "", io_ctx, false);
    int r = ictx->state->open(false);
    if (r < 0) {
      lderr(ictx->cct) << "error opening source image: " << cpp_strerror(r)
		       << dendl;
      delete ictx;
      return r;
    }
    BOOST_SCOPE_EXIT((ictx)) {
      ictx->state->close();
    } BOOST_SCOPE_EXIT_END

    return ictx->operations->rename(dstname);
  }

  int info(ImageCtx *ictx, image_info_t& info, size_t infosize)
  {
    ldout(ictx->cct, 20) << "info " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    image_info(ictx, info, infosize);
    return 0;
  }

  int get_old_format(ImageCtx *ictx, uint8_t *old)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;
    *old = ictx->old_format;
    return 0;
  }

  int get_size(ImageCtx *ictx, uint64_t *size)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;
    RWLock::RLocker l2(ictx->snap_lock);
    *size = ictx->get_image_size(ictx->snap_id);
    return 0;
  }

  int get_features(ImageCtx *ictx, uint64_t *features)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;
    RWLock::RLocker l(ictx->snap_lock);
    *features = ictx->features;
    return 0;
  }

  int get_overlap(ImageCtx *ictx, uint64_t *overlap)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;
    RWLock::RLocker l(ictx->snap_lock);
    RWLock::RLocker l2(ictx->parent_lock);
    return ictx->get_parent_overlap(ictx->snap_id, overlap);
  }

  int get_parent_info(ImageCtx *ictx, string *parent_pool_name,
		      string *parent_name, string *parent_snap_name)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    RWLock::RLocker l2(ictx->parent_lock);
    if (ictx->parent == NULL) {
      return -ENOENT;
    }

    parent_spec parent_spec;

    if (ictx->snap_id == CEPH_NOSNAP) {
      parent_spec = ictx->parent_md.spec;
    } else {
      r = ictx->get_parent_spec(ictx->snap_id, &parent_spec);
      if (r < 0) {
	lderr(ictx->cct) << "Can't find snapshot id = " << ictx->snap_id << dendl;
	return r;
      }
      if (parent_spec.pool_id == -1)
	return -ENOENT;
    }
    if (parent_pool_name) {
      Rados rados(ictx->md_ctx);
      r = rados.pool_reverse_lookup(parent_spec.pool_id,
				    parent_pool_name);
      if (r < 0) {
	lderr(ictx->cct) << "error looking up pool name: " << cpp_strerror(r)
			 << dendl;
	return r;
      }
    }

    if (parent_snap_name) {
      RWLock::RLocker l(ictx->parent->snap_lock);
      r = ictx->parent->get_snap_name(parent_spec.snap_id,
				      parent_snap_name);
      if (r < 0) {
	lderr(ictx->cct) << "error finding parent snap name: "
			 << cpp_strerror(r) << dendl;
	return r;
      }
    }

    if (parent_name) {
      r = cls_client::dir_get_name(&ictx->parent->md_ctx, RBD_DIRECTORY,
				   parent_spec.image_id, parent_name);
      if (r < 0) {
	lderr(ictx->cct) << "error getting parent image name: "
			 << cpp_strerror(r) << dendl;
	return r;
      }
    }

    return 0;
  }

  int get_flags(ImageCtx *ictx, uint64_t *flags)
  {
    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    RWLock::RLocker l2(ictx->snap_lock);
    return ictx->get_flags(ictx->snap_id, flags);
  }

  int set_image_notification(ImageCtx *ictx, int fd, int type)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << " " << ictx << " fd " << fd << " type" << type << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    if (ictx->event_socket.is_valid())
      return -EINVAL;
    return ictx->event_socket.init(fd, type);
  }

  int is_exclusive_lock_owner(ImageCtx *ictx, bool *is_owner)
  {
    *is_owner = false;

    RWLock::RLocker owner_locker(ictx->owner_lock);
    if (ictx->exclusive_lock == nullptr ||
        !ictx->exclusive_lock->is_lock_owner()) {
      return 0;
    }

    // might have been blacklisted by peer -- ensure we still own
    // the lock by pinging the OSD
    int r = ictx->exclusive_lock->assert_header_locked();
    if (r < 0) {
      return r;
    }

    *is_owner = true;
    return 0;
  }

  int lock_acquire(ImageCtx *ictx, rbd_lock_mode_t lock_mode)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << ", "
                   << "lock_mode=" << lock_mode << dendl;

    if (lock_mode != RBD_LOCK_MODE_EXCLUSIVE) {
      return -EOPNOTSUPP;
    }

    C_SaferCond lock_ctx;
    {
      RWLock::WLocker l(ictx->owner_lock);

      if (ictx->exclusive_lock == nullptr) {
	lderr(cct) << "exclusive-lock feature is not enabled" << dendl;
	return -EINVAL;
      }

      if (ictx->get_exclusive_lock_policy()->may_auto_request_lock()) {
	ictx->set_exclusive_lock_policy(
	  new exclusive_lock::StandardPolicy(ictx));
      }

      if (ictx->exclusive_lock->is_lock_owner()) {
	return 0;
      }

      ictx->exclusive_lock->request_lock(&lock_ctx);
    }

    int r = lock_ctx.wait();
    if (r < 0) {
      lderr(cct) << "failed to request exclusive lock: " << cpp_strerror(r)
		 << dendl;
      return r;
    }

    RWLock::RLocker l(ictx->owner_lock);

    if (ictx->exclusive_lock == nullptr ||
	!ictx->exclusive_lock->is_lock_owner()) {
      lderr(cct) << "failed to acquire exclusive lock" << dendl;
      return -EROFS;
    }

    return 0;
  }

  int lock_release(ImageCtx *ictx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;

    C_SaferCond lock_ctx;
    {
      RWLock::WLocker l(ictx->owner_lock);

      if (ictx->exclusive_lock == nullptr ||
	  !ictx->exclusive_lock->is_lock_owner()) {
	lderr(cct) << "not exclusive lock owner" << dendl;
	return -EINVAL;
      }

      ictx->exclusive_lock->release_lock(&lock_ctx);
    }

    int r = lock_ctx.wait();
    if (r < 0) {
      lderr(cct) << "failed to release exclusive lock: " << cpp_strerror(r)
		 << dendl;
      return r;
    }
    return 0;
  }

  int lock_get_owners(ImageCtx *ictx, rbd_lock_mode_t *lock_mode,
                      std::list<std::string> *lock_owners)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;

    exclusive_lock::Locker locker;
    C_SaferCond get_owner_ctx;
    auto get_owner_req = exclusive_lock::GetLockerRequest<>::create(
      *ictx, &locker, &get_owner_ctx);
    get_owner_req->send();

    int r = get_owner_ctx.wait();
    if (r == -ENOENT) {
      return r;
    } else if (r < 0) {
      lderr(cct) << "failed to determine current lock owner: "
                 << cpp_strerror(r) << dendl;
      return r;
    }

    *lock_mode = RBD_LOCK_MODE_EXCLUSIVE;
    lock_owners->clear();
    lock_owners->emplace_back(locker.address);
    return 0;
  }

  int lock_break(ImageCtx *ictx, rbd_lock_mode_t lock_mode,
                 const std::string &lock_owner)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << ", "
                   << "lock_mode=" << lock_mode << ", "
                   << "lock_owner=" << lock_owner << dendl;

    if (lock_mode != RBD_LOCK_MODE_EXCLUSIVE) {
      return -EOPNOTSUPP;
    }

    exclusive_lock::Locker locker;
    C_SaferCond get_owner_ctx;
    auto get_owner_req = exclusive_lock::GetLockerRequest<>::create(
      *ictx, &locker, &get_owner_ctx);
    get_owner_req->send();

    int r = get_owner_ctx.wait();
    if (r == -ENOENT) {
      return r;
    } else if (r < 0) {
      lderr(cct) << "failed to determine current lock owner: "
                 << cpp_strerror(r) << dendl;
      return r;
    }

    if (locker.address != lock_owner) {
      return -EBUSY;
    }

    C_SaferCond break_ctx;
    auto break_req = exclusive_lock::BreakRequest<>::create(
      *ictx, locker, ictx->blacklist_on_break_lock, true, &break_ctx);
    break_req->send();

    r = break_ctx.wait();
    if (r == -ENOENT) {
      return r;
    } else if (r < 0) {
      lderr(cct) << "failed to break lock: " << cpp_strerror(r) << dendl;
      return r;
    }
    return 0;
  }

  int remove(IoCtx& io_ctx, const std::string &image_name,
             const std::string &image_id, ProgressContext& prog_ctx,
             bool force)
  {
    CephContext *cct((CephContext *)io_ctx.cct());
    ldout(cct, 20) << "remove " << &io_ctx << " "
                   << (image_id.empty() ? image_name : image_id) << dendl;

    std::string name(image_name);
    std::string id(image_id);
    bool old_format = false;
    bool unknown_format = true;
    ImageCtx *ictx = new ImageCtx(
      (id.empty() ? name : std::string()), id, nullptr, io_ctx, false);
    int r = ictx->state->open(true);
    if (r < 0) {
      ldout(cct, 2) << "error opening image: " << cpp_strerror(-r) << dendl;
      delete ictx;
      if (r != -ENOENT) {
	return r;
      }
    } else {
      string header_oid = ictx->header_oid;
      old_format = ictx->old_format;
      unknown_format = false;
      name = ictx->name;
      id = ictx->id;

      ictx->owner_lock.get_read();
      if (ictx->exclusive_lock != nullptr) {
        if (force) {
          // releasing read lock to avoid a deadlock when upgrading to
          // write lock in the shut_down process
          ictx->owner_lock.put_read();
          if (ictx->exclusive_lock != nullptr) {
            C_SaferCond ctx;
            ictx->exclusive_lock->shut_down(&ctx);
            r = ctx.wait();
            if (r < 0) {
              lderr(cct) << "error shutting down exclusive lock: "
                         << cpp_strerror(r) << dendl;
              ictx->state->close();
              return r;
            }
            assert (ictx->exclusive_lock == nullptr);
            ictx->owner_lock.get_read();
          }
        } else {
          r = ictx->operations->prepare_image_update();
          if (r < 0 || !ictx->exclusive_lock->is_lock_owner()) {
	    lderr(cct) << "cannot obtain exclusive lock - not removing" << dendl;
	    ictx->owner_lock.put_read();
	    ictx->state->close();
            return -EBUSY;
          }
        }
      }

      if (ictx->snaps.size()) {
	lderr(cct) << "image has snapshots - not removing" << dendl;
	ictx->owner_lock.put_read();
	ictx->state->close();
	return -ENOTEMPTY;
      }

      std::list<obj_watch_t> watchers;
      r = io_ctx.list_watchers(header_oid, &watchers);
      if (r < 0) {
        lderr(cct) << "error listing watchers" << dendl;
	ictx->owner_lock.put_read();
        ictx->state->close();
        return r;
      }
      if (watchers.size() > 1) {
        lderr(cct) << "image has watchers - not removing" << dendl;
	ictx->owner_lock.put_read();
        ictx->state->close();
        return -EBUSY;
      }

      cls::rbd::GroupSpec s;
      r = cls_client::image_get_group(&io_ctx, header_oid, &s);
      if (r < 0 && r != -EOPNOTSUPP) {
        lderr(cct) << "error querying consistency group" << dendl;
        ictx->owner_lock.put_read();
        ictx->state->close();
        return r;
      } else if (s.is_valid()) {
	lderr(cct) << "image is in a consistency group - not removing" << dendl;
	ictx->owner_lock.put_read();
	ictx->state->close();
	return -EMLINK;
      }

      trim_image(ictx, 0, prog_ctx);

      ictx->parent_lock.get_read();
      // struct assignment
      parent_info parent_info = ictx->parent_md;
      ictx->parent_lock.put_read();

      r = cls_client::remove_child(&ictx->md_ctx, RBD_CHILDREN,
				   parent_info.spec, id);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing child from children list" << dendl;
	ictx->owner_lock.put_read();
        ictx->state->close();
	return r;
      }

      if (!old_format) {
        r = mirror_image_disable_internal(ictx, force, !force);
        if (r < 0 && r != -EOPNOTSUPP) {
          lderr(cct) << "error disabling image mirroring: " << cpp_strerror(r)
                     << dendl;
          ictx->owner_lock.put_read();
          ictx->state->close();
          return r;
        }
      }

      ictx->owner_lock.put_read();
      ictx->state->close();

      ldout(cct, 2) << "removing header..." << dendl;
      r = io_ctx.remove(header_oid);
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing header: " << cpp_strerror(-r) << dendl;
	return r;
      }
    }

    if (old_format || unknown_format) {
      ldout(cct, 2) << "removing rbd image from v1 directory..." << dendl;
      r = tmap_rm(io_ctx, name);
      old_format = (r == 0);
      if (r < 0 && !unknown_format) {
        if (r != -ENOENT) {
          lderr(cct) << "error removing image from v1 directory: "
                     << cpp_strerror(-r) << dendl;
        }
	return r;
      }
    }
    if (!old_format) {
      if (id.empty()) {
        ldout(cct, 5) << "attempting to determine image id" << dendl;
        r = cls_client::dir_get_id(&io_ctx, RBD_DIRECTORY, name, &id);
        if (r < 0 && r != -ENOENT) {
          lderr(cct) << "error getting id of image" << dendl;
          return r;
        }
      } else if (name.empty()) {
        ldout(cct, 5) << "attempting to determine image name" << dendl;
        r = cls_client::dir_get_name(&io_ctx, RBD_DIRECTORY, id, &name);
        if (r < 0 && r != -ENOENT) {
          lderr(cct) << "error getting name of image" << dendl;
          return r;
        }
      }

      if (!id.empty()) {
        ldout(cct, 10) << "removing journal..." << dendl;
        r = Journal<>::remove(io_ctx, id);
        if (r < 0 && r != -ENOENT) {
          lderr(cct) << "error removing image journal" << dendl;
          return r;
        }

        ldout(cct, 10) << "removing object map..." << dendl;
        r = ObjectMap<>::remove(io_ctx, id);
        if (r < 0 && r != -ENOENT) {
          lderr(cct) << "error removing image object map" << dendl;
          return r;
        }

        ldout(cct, 10) << "removing image from rbd_mirroring object..."
                       << dendl;
        r = cls_client::mirror_image_remove(&io_ctx, id);
        if (r < 0 && r != -ENOENT && r != -EOPNOTSUPP) {
          lderr(cct) << "failed to remove image from mirroring directory: "
                     << cpp_strerror(r) << dendl;
          return r;
        }
      }

      ldout(cct, 2) << "removing id object..." << dendl;
      r = io_ctx.remove(util::id_obj_name(name));
      if (r < 0 && r != -ENOENT) {
	lderr(cct) << "error removing id object: " << cpp_strerror(r)
                   << dendl;
	return r;
      }

      ldout(cct, 2) << "removing rbd image from v2 directory..." << dendl;
      r = cls_client::dir_remove_image(&io_ctx, RBD_DIRECTORY, name, id);
      if (r < 0) {
        if (r != -ENOENT) {
          lderr(cct) << "error removing image from v2 directory: "
                     << cpp_strerror(-r) << dendl;
        }
        return r;
      }
    }

    ldout(cct, 2) << "done." << dendl;
    return 0;
  }

  int snap_list(ImageCtx *ictx, vector<snap_info_t>& snaps)
  {
    ldout(ictx->cct, 20) << "snap_list " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    for (map<snap_t, SnapInfo>::iterator it = ictx->snap_info.begin();
	 it != ictx->snap_info.end(); ++it) {
      snap_info_t info;
      info.name = it->second.name;
      info.id = it->first;
      info.size = it->second.size;
      snaps.push_back(info);
    }

    return 0;
  }

  int snap_exists(ImageCtx *ictx, const char *snap_name, bool *exists)
  {
    ldout(ictx->cct, 20) << "snap_exists " << ictx << " " << snap_name << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker l(ictx->snap_lock);
    *exists = ictx->get_snap_id(snap_name) != CEPH_NOSNAP; 
    return 0;
  }

  int snap_remove(ImageCtx *ictx, const char *snap_name, uint32_t flags, ProgressContext& pctx)
  {
    ldout(ictx->cct, 20) << "snap_remove " << ictx << " " << snap_name << " flags: " << flags << dendl;

    int r = 0;

    cls::rbd::SnapshotNamespace snap_namespace;
    r = get_snap_namespace(ictx, snap_name, &snap_namespace);
    if (r < 0) {
      return r;
    }
    if (boost::get<cls::rbd::UserSnapshotNamespace>(&snap_namespace) == nullptr) {
      return -EINVAL;
    }

    r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    if (flags & RBD_SNAP_REMOVE_FLATTEN) {
	r = flatten_children(ictx, snap_name, pctx);
	if (r < 0) {
	  return r;
	}
    }

    bool is_protected;
    r = snap_is_protected(ictx, snap_name, &is_protected);
    if (r < 0) {
      return r;
    }

    if (is_protected && flags & RBD_SNAP_REMOVE_UNPROTECT) {
      r = ictx->operations->snap_unprotect(snap_name);
      if (r < 0) {
	lderr(ictx->cct) << "failed to unprotect snapshot: " << snap_name << dendl;
	return r;
      }

      r = snap_is_protected(ictx, snap_name, &is_protected);
      if (r < 0) {
	return r;
      }
      if (is_protected) {
	lderr(ictx->cct) << "snapshot is still protected after unprotection" << dendl;
	ceph_abort();
      }
    }

    C_SaferCond ctx;
    ictx->operations->snap_remove(snap_name, &ctx);

    r = ctx.wait();
    return r;
  }

  int snap_get_limit(ImageCtx *ictx, uint64_t *limit)
  {
    int r = cls_client::snapshot_get_limit(&ictx->md_ctx, ictx->header_oid,
                                           limit);
    if (r == -EOPNOTSUPP) {
      *limit = UINT64_MAX;
      r = 0;
    }
    return r;
  }

  int snap_set_limit(ImageCtx *ictx, uint64_t limit)
  {
    return ictx->operations->snap_set_limit(limit);
  }

  struct CopyProgressCtx {
    explicit CopyProgressCtx(ProgressContext &p)
      : destictx(NULL), src_size(0), prog_ctx(p)
    { }

    ImageCtx *destictx;
    uint64_t src_size;
    ProgressContext &prog_ctx;
  };

  int copy(ImageCtx *src, IoCtx& dest_md_ctx, const char *destname,
	   ImageOptions& opts, ProgressContext &prog_ctx)
  {
    CephContext *cct = (CephContext *)dest_md_ctx.cct();
    ldout(cct, 20) << "copy " << src->name
		   << (src->snap_name.length() ? "@" + src->snap_name : "")
		   << " -> " << destname << " opts = " << opts << dendl;

    src->snap_lock.get_read();
    uint64_t features = src->features;
    uint64_t src_size = src->get_image_size(src->snap_id);
    src->snap_lock.put_read();
    uint64_t format = src->old_format ? 1 : 2;
    if (opts.get(RBD_IMAGE_OPTION_FORMAT, &format) != 0) {
      opts.set(RBD_IMAGE_OPTION_FORMAT, format);
    }
    uint64_t stripe_unit = src->stripe_unit;
    if (opts.get(RBD_IMAGE_OPTION_STRIPE_UNIT, &stripe_unit) != 0) {
      opts.set(RBD_IMAGE_OPTION_STRIPE_UNIT, stripe_unit);
    }
    uint64_t stripe_count = src->stripe_count;
    if (opts.get(RBD_IMAGE_OPTION_STRIPE_COUNT, &stripe_count) != 0) {
      opts.set(RBD_IMAGE_OPTION_STRIPE_COUNT, stripe_count);
    }
    uint64_t order = src->order;
    if (opts.get(RBD_IMAGE_OPTION_ORDER, &order) != 0) {
      opts.set(RBD_IMAGE_OPTION_ORDER, order);
    }
    if (opts.get(RBD_IMAGE_OPTION_FEATURES, &features) != 0) {
      opts.set(RBD_IMAGE_OPTION_FEATURES, features);
    }
    if (features & ~RBD_FEATURES_ALL) {
      lderr(cct) << "librbd does not support requested features" << dendl;
      return -ENOSYS;
    }

    int r = create(dest_md_ctx, destname, src_size, opts, "", "", false);
    if (r < 0) {
      lderr(cct) << "header creation failed" << dendl;
      return r;
    }
    opts.set(RBD_IMAGE_OPTION_ORDER, static_cast<uint64_t>(order));

    ImageCtx *dest = new librbd::ImageCtx(destname, "", NULL,
					  dest_md_ctx, false);
    r = dest->state->open(false);
    if (r < 0) {
      delete dest;
      lderr(cct) << "failed to read newly created header" << dendl;
      return r;
    }

    r = copy(src, dest, prog_ctx);
    int close_r = dest->state->close();
    if (r == 0 && close_r < 0) {
      r = close_r;
    }
    return r;
  }

  class C_CopyWrite : public Context {
  public:
    C_CopyWrite(SimpleThrottle *throttle, bufferlist *bl)
      : m_throttle(throttle), m_bl(bl) {}
    virtual void finish(int r) {
      delete m_bl;
      m_throttle->end_op(r);
    }
  private:
    SimpleThrottle *m_throttle;
    bufferlist *m_bl;
  };

  class C_CopyRead : public Context {
  public:
    C_CopyRead(SimpleThrottle *throttle, ImageCtx *dest, uint64_t offset,
	       bufferlist *bl)
      : m_throttle(throttle), m_dest(dest), m_offset(offset), m_bl(bl) {
      m_throttle->start_op();
    }
    virtual void finish(int r) {
      if (r < 0) {
	lderr(m_dest->cct) << "error reading from source image at offset "
			   << m_offset << ": " << cpp_strerror(r) << dendl;
	delete m_bl;
	m_throttle->end_op(r);
	return;
      }
      assert(m_bl->length() == (size_t)r);

      if (m_bl->is_zero()) {
	delete m_bl;
	m_throttle->end_op(r);
	return;
      }

      Context *ctx = new C_CopyWrite(m_throttle, m_bl);
      AioCompletion *comp = AioCompletion::create(ctx);

      // coordinate through AIO WQ to ensure lock is acquired if needed
      m_dest->aio_work_queue->aio_write(comp, m_offset, m_bl->length(),
                                        m_bl->c_str(),
                                        LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    }

  private:
    SimpleThrottle *m_throttle;
    ImageCtx *m_dest;
    uint64_t m_offset;
    bufferlist *m_bl;
  };

  int copy(ImageCtx *src, ImageCtx *dest, ProgressContext &prog_ctx)
  {
    src->snap_lock.get_read();
    uint64_t src_size = src->get_image_size(src->snap_id);
    src->snap_lock.put_read();

    dest->snap_lock.get_read();
    uint64_t dest_size = dest->get_image_size(dest->snap_id);
    dest->snap_lock.put_read();

    CephContext *cct = src->cct;
    if (dest_size < src_size) {
      lderr(cct) << " src size " << src_size << " > dest size "
		 << dest_size << dendl;
      return -EINVAL;
    }
    int r;
    map<string, bufferlist> pairs;

    r = cls_client::metadata_list(&src->md_ctx, src->header_oid, "", 0, &pairs);
    if (r < 0 && r != -EOPNOTSUPP && r != -EIO) {
      lderr(cct) << "couldn't list metadata: " << cpp_strerror(r) << dendl;
      return r;
    } else if (r == 0 && !pairs.empty()) {
      r = cls_client::metadata_set(&dest->md_ctx, dest->header_oid, pairs);
      if (r < 0) {
        lderr(cct) << "couldn't set metadata: " << cpp_strerror(r) << dendl;
        return r;
      }
    }

    RWLock::RLocker owner_lock(src->owner_lock);
    SimpleThrottle throttle(src->concurrent_management_ops, false);
    uint64_t period = src->get_stripe_period();
    unsigned fadvise_flags = LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL | LIBRADOS_OP_FLAG_FADVISE_NOCACHE;
    for (uint64_t offset = 0; offset < src_size; offset += period) {
      if (throttle.pending_error()) {
        return throttle.wait_for_ret();
      }

      uint64_t len = min(period, src_size - offset);
      bufferlist *bl = new bufferlist();
      Context *ctx = new C_CopyRead(&throttle, dest, offset, bl);
      AioCompletion *comp = AioCompletion::create_and_start(ctx, src,
                                                            AIO_TYPE_READ);
      AioImageRequest<>::aio_read(src, comp, {{offset, len}}, nullptr, bl,
                                  fadvise_flags);
      prog_ctx.update_progress(offset, src_size);
    }

    r = throttle.wait_for_ret();
    if (r >= 0)
      prog_ctx.update_progress(src_size, src_size);
    return r;
  }

  int snap_set(ImageCtx *ictx, const char *snap_name)
  {
    ldout(ictx->cct, 20) << "snap_set " << ictx << " snap = "
			 << (snap_name ? snap_name : "NULL") << dendl;

    // ignore return value, since we may be set to a non-existent
    // snapshot and the user is trying to fix that
    ictx->state->refresh_if_required();

    C_SaferCond ctx;
    std::string name(snap_name == nullptr ? "" : snap_name);
    ictx->state->snap_set(name, &ctx);

    int r = ctx.wait();
    if (r < 0) {
      if (r != -ENOENT) {
        lderr(ictx->cct) << "failed to " << (name.empty() ? "un" : "") << "set "
                         << "snapshot: " << cpp_strerror(r) << dendl;
      }
      return r;
    }

    return 0;
  }

  int list_lockers(ImageCtx *ictx,
		   std::list<locker_t> *lockers,
		   bool *exclusive,
		   string *tag)
  {
    ldout(ictx->cct, 20) << "list_locks on image " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    RWLock::RLocker locker(ictx->md_lock);
    if (exclusive)
      *exclusive = ictx->exclusive_locked;
    if (tag)
      *tag = ictx->lock_tag;
    if (lockers) {
      lockers->clear();
      map<rados::cls::lock::locker_id_t,
	  rados::cls::lock::locker_info_t>::const_iterator it;
      for (it = ictx->lockers.begin(); it != ictx->lockers.end(); ++it) {
	locker_t locker;
	locker.client = stringify(it->first.locker);
	locker.cookie = it->first.cookie;
	locker.address = stringify(it->second.addr);
	lockers->push_back(locker);
      }
    }

    return 0;
  }

  int lock(ImageCtx *ictx, bool exclusive, const string& cookie,
	   const string& tag)
  {
    ldout(ictx->cct, 20) << "lock image " << ictx << " exclusive=" << exclusive
			 << " cookie='" << cookie << "' tag='" << tag << "'"
			 << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    /**
     * If we wanted we could do something more intelligent, like local
     * checks that we think we will succeed. But for now, let's not
     * duplicate that code.
     */
    {
      RWLock::RLocker locker(ictx->md_lock);
      r = rados::cls::lock::lock(&ictx->md_ctx, ictx->header_oid, RBD_LOCK_NAME,
			         exclusive ? LOCK_EXCLUSIVE : LOCK_SHARED,
			         cookie, tag, "", utime_t(), 0);
      if (r < 0) {
        return r;
      }
    }

    ictx->notify_update();
    return 0;
  }

  int unlock(ImageCtx *ictx, const string& cookie)
  {
    ldout(ictx->cct, 20) << "unlock image " << ictx
			 << " cookie='" << cookie << "'" << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    {
      RWLock::RLocker locker(ictx->md_lock);
      r = rados::cls::lock::unlock(&ictx->md_ctx, ictx->header_oid,
				   RBD_LOCK_NAME, cookie);
      if (r < 0) {
        return r;
      }
    }

    ictx->notify_update();
    return 0;
  }

  int break_lock(ImageCtx *ictx, const string& client,
		 const string& cookie)
  {
    ldout(ictx->cct, 20) << "break_lock image " << ictx << " client='" << client
			 << "' cookie='" << cookie << "'" << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    entity_name_t lock_client;
    if (!lock_client.parse(client)) {
      lderr(ictx->cct) << "Unable to parse client '" << client
		       << "'" << dendl;
      return -EINVAL;
    }

    if (ictx->blacklist_on_break_lock) {
      typedef std::map<rados::cls::lock::locker_id_t,
		       rados::cls::lock::locker_info_t> Lockers;
      Lockers lockers;
      ClsLockType lock_type;
      std::string lock_tag;
      r = rados::cls::lock::get_lock_info(&ictx->md_ctx, ictx->header_oid,
                                          RBD_LOCK_NAME, &lockers, &lock_type,
                                          &lock_tag);
      if (r < 0) {
        lderr(ictx->cct) << "unable to retrieve lock info: " << cpp_strerror(r)
          	       << dendl;
        return r;
      }

      std::string client_address;
      for (Lockers::iterator it = lockers.begin();
           it != lockers.end(); ++it) {
        if (it->first.locker == lock_client) {
          client_address = stringify(it->second.addr);
          break;
        }
      }
      if (client_address.empty()) {
        return -ENOENT;
      }

      RWLock::RLocker locker(ictx->md_lock);
      librados::Rados rados(ictx->md_ctx);
      r = rados.blacklist_add(client_address,
			      ictx->blacklist_expire_seconds);
      if (r < 0) {
        lderr(ictx->cct) << "unable to blacklist client: " << cpp_strerror(r)
          	       << dendl;
        return r;
      }
    }

    r = rados::cls::lock::break_lock(&ictx->md_ctx, ictx->header_oid,
				     RBD_LOCK_NAME, cookie, lock_client);
    if (r < 0)
      return r;
    ictx->notify_update();
    return 0;
  }

  void rbd_ctx_cb(completion_t cb, void *arg)
  {
    Context *ctx = reinterpret_cast<Context *>(arg);
    AioCompletion *comp = reinterpret_cast<AioCompletion *>(cb);
    ctx->complete(comp->get_return_value());
    comp->release();
  }

  int64_t read_iterate(ImageCtx *ictx, uint64_t off, uint64_t len,
		       int (*cb)(uint64_t, size_t, const char *, void *),
		       void *arg)
  {
    utime_t start_time, elapsed;

    ldout(ictx->cct, 20) << "read_iterate " << ictx << " off = " << off
			 << " len = " << len << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0)
      return r;

    uint64_t mylen = len;
    ictx->snap_lock.get_read();
    r = clip_io(ictx, off, &mylen);
    ictx->snap_lock.put_read();
    if (r < 0)
      return r;

    int64_t total_read = 0;
    uint64_t period = ictx->get_stripe_period();
    uint64_t left = mylen;

    RWLock::RLocker owner_locker(ictx->owner_lock);
    start_time = ceph_clock_now();
    while (left > 0) {
      uint64_t period_off = off - (off % period);
      uint64_t read_len = min(period_off + period - off, left);

      bufferlist bl;

      C_SaferCond ctx;
      AioCompletion *c = AioCompletion::create_and_start(&ctx, ictx,
                                                         AIO_TYPE_READ);
      AioImageRequest<>::aio_read(ictx, c, {{off, read_len}}, nullptr, &bl, 0);

      int ret = ctx.wait();
      if (ret < 0) {
        return ret;
      }

      r = cb(total_read, ret, bl.c_str(), arg);
      if (r < 0) {
	return r;
      }

      total_read += ret;
      left -= ret;
      off += ret;
    }

    elapsed = ceph_clock_now() - start_time;
    ictx->perfcounter->tinc(l_librbd_rd_latency, elapsed);
    ictx->perfcounter->inc(l_librbd_rd);
    ictx->perfcounter->inc(l_librbd_rd_bytes, mylen);
    return total_read;
  }

  int diff_iterate(ImageCtx *ictx, const char *fromsnapname, uint64_t off,
                   uint64_t len, bool include_parent, bool whole_object,
		   int (*cb)(uint64_t, size_t, int, void *), void *arg)
  {
    ldout(ictx->cct, 20) << "diff_iterate " << ictx << " off = " << off
			 << " len = " << len << dendl;

    // ensure previous writes are visible to listsnaps
    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      ictx->flush();
    }

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    ictx->snap_lock.get_read();
    r = clip_io(ictx, off, &len);
    ictx->snap_lock.put_read();
    if (r < 0) {
      return r;
    }

    DiffIterate command(*ictx, fromsnapname, off, len, include_parent,
                        whole_object, cb, arg);
    r = command.execute();
    return r;
  }

  // validate extent against image size; clip to image size if necessary
  int clip_io(ImageCtx *ictx, uint64_t off, uint64_t *len)
  {
    assert(ictx->snap_lock.is_locked());
    uint64_t image_size = ictx->get_image_size(ictx->snap_id);
    bool snap_exists = ictx->snap_exists;

    if (!snap_exists)
      return -ENOENT;

    // special-case "len == 0" requests: always valid
    if (*len == 0)
      return 0;

    // can't start past end
    if (off >= image_size)
      return -EINVAL;

    // clip requests that extend past end to just end
    if ((off + *len) > image_size)
      *len = (size_t)(image_size - off);

    return 0;
  }

  int flush(ImageCtx *ictx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "flush " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    ictx->user_flushed();
    C_SaferCond ctx;
    {
      RWLock::RLocker owner_locker(ictx->owner_lock);
      ictx->flush(&ctx);
    }
    r = ctx.wait();

    ictx->perfcounter->inc(l_librbd_flush);
    return r;
  }

  int invalidate_cache(ImageCtx *ictx)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "invalidate_cache " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    RWLock::RLocker owner_locker(ictx->owner_lock);
    RWLock::WLocker md_locker(ictx->md_lock);
    r = ictx->invalidate_cache(false);
    ictx->perfcounter->inc(l_librbd_invalidate_cache);
    return r;
  }

  int poll_io_events(ImageCtx *ictx, AioCompletion **comps, int numcomp)
  {
    if (numcomp <= 0)
      return -EINVAL;
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << " " << ictx << " numcomp = " << numcomp << dendl;
    int i = 0;
    Mutex::Locker l(ictx->completed_reqs_lock);
    while (i < numcomp) {
      if (ictx->completed_reqs.empty())
        break;
      comps[i++] = ictx->completed_reqs.front();
      ictx->completed_reqs.pop_front();
    }
    return i;
  }

  int metadata_get(ImageCtx *ictx, const string &key, string *value)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_get " << ictx << " key=" << key << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    return cls_client::metadata_get(&ictx->md_ctx, ictx->header_oid, key, value);
  }

  int metadata_list(ImageCtx *ictx, const string &start, uint64_t max, map<string, bufferlist> *pairs)
  {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "metadata_list " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    return cls_client::metadata_list(&ictx->md_ctx, ictx->header_oid, start, max, pairs);
  }

  int mirror_image_enable(ImageCtx *ictx, bool relax_same_pool_parent_check) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "mirror_image_enable " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    cls::rbd::MirrorMode mirror_mode;
    r = cls_client::mirror_mode_get(&ictx->md_ctx, &mirror_mode);
    if (r < 0) {
      lderr(cct) << "cannot enable mirroring: failed to retrieve mirror mode: "
        << cpp_strerror(r) << dendl;
      return r;
    }

    if (mirror_mode != cls::rbd::MIRROR_MODE_IMAGE) {
      lderr(cct) << "cannot enable mirroring in the current pool mirroring "
        "mode" << dendl;
      return -EINVAL;
    }

    // is mirroring not enabled for the parent?
    {
      RWLock::RLocker l(ictx->parent_lock);
      ImageCtx *parent = ictx->parent;
      if (parent) {
        if (relax_same_pool_parent_check &&
            parent->md_ctx.get_id() == ictx->md_ctx.get_id()) {
          if (!parent->test_features(RBD_FEATURE_JOURNALING)) {
            lderr(cct) << "journaling is not enabled for the parent" << dendl;
            return -EINVAL;
          }
        } else {
          cls::rbd::MirrorImage mirror_image_internal;
          r = cls_client::mirror_image_get(&(parent->md_ctx), parent->id,
                                           &mirror_image_internal);
          if (r == -ENOENT) {
            lderr(cct) << "mirroring is not enabled for the parent" << dendl;
            return -EINVAL;
          }
        }
      }
    }

    r = mirror_image_enable_internal(ictx);
    if (r < 0) {
      return r;
    }
    return 0;
  }

  int mirror_image_disable(ImageCtx *ictx, bool force) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << "mirror_image_disable " << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    cls::rbd::MirrorMode mirror_mode;
    r = cls_client::mirror_mode_get(&ictx->md_ctx, &mirror_mode);
    if (r < 0) {
      lderr(cct) << "cannot disable mirroring: failed to retrieve pool "
        "mirroring mode: " << cpp_strerror(r) << dendl;
      return r;
    }

    if (mirror_mode != cls::rbd::MIRROR_MODE_IMAGE) {
      lderr(cct) << "cannot disable mirroring in the current pool mirroring "
        "mode" << dendl;
      return -EINVAL;
    }

    // is mirroring  enabled for the child?
    cls::rbd::MirrorImage mirror_image_internal;
    r = cls_client::mirror_image_get(&ictx->md_ctx, ictx->id, &mirror_image_internal);
    if (r == -ENOENT) {
      // mirroring is not enabled for this image
      ldout(cct, 20) << "ignoring disable command: mirroring is not enabled for this image" 
                     << dendl;
      return 0;
    } else if (r == -EOPNOTSUPP) {
      ldout(cct, 5) << "mirroring not supported by OSD" << dendl;
      return r;
    } else if (r < 0) {
      lderr(cct) << "failed to retrieve mirror image metadata: " << cpp_strerror(r) << dendl;
      return r;
    }
    mirror_image_internal.state = cls::rbd::MIRROR_IMAGE_STATE_DISABLING;
    r = cls_client::mirror_image_set(&ictx->md_ctx, ictx->id, mirror_image_internal);
    if (r < 0) {
      lderr(cct) << "cannot disable mirroring: " << cpp_strerror(r) << dendl;
      return r;
    } else {
      bool rollback = false;
      BOOST_SCOPE_EXIT_ALL(ictx, &mirror_image_internal, &rollback) {
        if (rollback) {
          CephContext *cct = ictx->cct;
          mirror_image_internal.state = cls::rbd::MIRROR_IMAGE_STATE_ENABLED;
          int r = cls_client::mirror_image_set(&ictx->md_ctx, ictx->id, mirror_image_internal);
          if (r < 0) {
            lderr(cct) << "failed to re-enable image mirroring: " << cpp_strerror(r) 
                       << dendl;          
          }
        }
      };

      {
        RWLock::RLocker l(ictx->snap_lock);
        map<librados::snap_t, SnapInfo> snap_info = ictx->snap_info;
        for (auto &info : snap_info) {
          librbd::parent_spec parent_spec(ictx->md_ctx.get_id(), ictx->id, info.first);
          map< pair<int64_t, string>, set<string> > image_info;

          r = list_children_info(ictx, parent_spec, image_info);
          if (r < 0) {
            rollback = true;
            return r;
          }
          if (image_info.empty())
            continue;

          Rados rados(ictx->md_ctx);
          for (auto &info: image_info) {
            IoCtx ioctx;
            r = rados.ioctx_create2(info.first.first, ioctx);
            if (r < 0) {
              rollback = true;
              lderr(cct) << "Error accessing child image pool " << info.first.second  << dendl;
              return r;
            }
            for (auto &id_it : info.second) {
              cls::rbd::MirrorImage mirror_image_internal;
              r = cls_client::mirror_image_get(&ioctx, id_it, &mirror_image_internal);
              if (r != -ENOENT) {
                rollback = true;
                lderr(cct) << "mirroring is enabled on one or more children " << dendl;
                return -EBUSY;
              }
            }
          }
        }
      }

      r = mirror_image_disable_internal(ictx, force);
      if (r < 0) {
        rollback = true;
        return r;
      }
    }

    return 0;
  }

  int mirror_image_promote(ImageCtx *ictx, bool force) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << ", "
                   << "force=" << force << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    r = validate_mirroring_enabled(ictx);
    if (r < 0) {
      return r;
    }

    std::string mirror_uuid;
    r = Journal<>::get_tag_owner(ictx, &mirror_uuid);
    if (r < 0) {
      lderr(cct) << "failed to determine tag ownership: " << cpp_strerror(r)
                 << dendl;
      return r;
    } else if (mirror_uuid == Journal<>::LOCAL_MIRROR_UUID) {
      lderr(cct) << "image is already primary" << dendl;
      return -EINVAL;
    } else if (mirror_uuid != Journal<>::ORPHAN_MIRROR_UUID && !force) {
      lderr(cct) << "image is still primary within a remote cluster" << dendl;
      return -EBUSY;
    }

    // TODO: need interlock with local rbd-mirror daemon to ensure it has stopped
    //       replay

    r = Journal<>::promote(ictx);
    if (r < 0) {
      lderr(cct) << "failed to promote image: " << cpp_strerror(r)
                 << dendl;
      return r;
    }
    return 0;
  }

  int mirror_image_demote(ImageCtx *ictx) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    r = validate_mirroring_enabled(ictx);
    if (r < 0) {
      return r;
    }

    bool is_primary;
    r = Journal<>::is_tag_owner(ictx, &is_primary);
    if (r < 0) {
      lderr(cct) << "failed to determine tag ownership: " << cpp_strerror(r)
                 << dendl;
      return r;
    }

    if (!is_primary) {
      lderr(cct) << "image is not currently the primary" << dendl;
      return -EINVAL;
    }

    RWLock::RLocker owner_lock(ictx->owner_lock);
    if (ictx->exclusive_lock == nullptr) {
      lderr(cct) << "exclusive lock is not active" << dendl;
      return -EINVAL;
    }

    // avoid accepting new requests from peers while we demote
    // the image
    ictx->exclusive_lock->block_requests(0);
    BOOST_SCOPE_EXIT_ALL( (ictx) ) {
      if (ictx->exclusive_lock != nullptr) {
        ictx->exclusive_lock->unblock_requests();
      }
    };

    C_SaferCond lock_ctx;
    ictx->exclusive_lock->request_lock(&lock_ctx);

    // don't block holding lock since refresh might be required
    ictx->owner_lock.put_read();
    r = lock_ctx.wait();
    ictx->owner_lock.get_read();

    if (r < 0) {
      lderr(cct) << "failed to lock image: " << cpp_strerror(r) << dendl;
      return r;
    } else if (ictx->exclusive_lock == nullptr ||
               !ictx->exclusive_lock->is_lock_owner()) {
      lderr(cct) << "failed to acquire exclusive lock" << dendl;
      return -EROFS;
    }

    BOOST_SCOPE_EXIT_ALL( (ictx) ) {
      C_SaferCond lock_ctx;
      ictx->exclusive_lock->release_lock(&lock_ctx);
      lock_ctx.wait();
    };

    RWLock::RLocker snap_locker(ictx->snap_lock);
    if (ictx->journal == nullptr) {
      lderr(cct) << "journal is not active" << dendl;
      return -EINVAL;
    } else if (!ictx->journal->is_tag_owner()) {
      lderr(cct) << "image is not currently the primary" << dendl;
      return -EINVAL;
    }

    r = ictx->journal->demote();
    if (r < 0) {
      lderr(cct) << "failed to demote image: " << cpp_strerror(r)
                 << dendl;
      return r;
    }
    return 0;
  }

  int mirror_image_resync(ImageCtx *ictx) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    r = validate_mirroring_enabled(ictx);
    if (r < 0) {
      return r;
    }

    std::string mirror_uuid;
    r = Journal<>::get_tag_owner(ictx, &mirror_uuid);
    if (r < 0) {
      lderr(cct) << "failed to determine tag ownership: " << cpp_strerror(r)
                 << dendl;
      return r;
    } else if (mirror_uuid == Journal<>::LOCAL_MIRROR_UUID) {
      lderr(cct) << "image is primary, cannot resync to itself" << dendl;
      return -EINVAL;
    }

    // flag the journal indicating that we want to rebuild the local image
    r = Journal<>::request_resync(ictx);
    if (r < 0) {
      lderr(cct) << "failed to request resync: " << cpp_strerror(r) << dendl;
      return r;
    }

    return 0;
  }

  int mirror_image_get_info(ImageCtx *ictx, mirror_image_info_t *mirror_image_info,
                            size_t info_size) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;
    if (info_size < sizeof(mirror_image_info_t)) {
      return -ERANGE;
    }

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    cls::rbd::MirrorImage mirror_image_internal;
    r = cls_client::mirror_image_get(&ictx->md_ctx, ictx->id,
        &mirror_image_internal);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "failed to retrieve mirroring state: " << cpp_strerror(r)
        << dendl;
      return r;
    }

    mirror_image_info->global_id = mirror_image_internal.global_image_id;
    if (r == -ENOENT) {
      mirror_image_info->state = RBD_MIRROR_IMAGE_DISABLED;
    } else {
      mirror_image_info->state =
        static_cast<rbd_mirror_image_state_t>(mirror_image_internal.state);
    }

    if (mirror_image_info->state == RBD_MIRROR_IMAGE_ENABLED) {
      r = Journal<>::is_tag_owner(ictx, &mirror_image_info->primary);
      if (r < 0) {
        lderr(cct) << "failed to check tag ownership: "
                   << cpp_strerror(r) << dendl;
        return r;
      }
    } else {
      mirror_image_info->primary = false;
    }

    return 0;
  }

  int mirror_image_get_status(ImageCtx *ictx, mirror_image_status_t *status,
			      size_t status_size) {
    CephContext *cct = ictx->cct;
    ldout(cct, 20) << __func__ << ": ictx=" << ictx << dendl;
    if (status_size < sizeof(mirror_image_status_t)) {
      return -ERANGE;
    }

    int r = ictx->state->refresh_if_required();
    if (r < 0) {
      return r;
    }

    mirror_image_info_t info;
    r = mirror_image_get_info(ictx, &info, sizeof(info));
    if (r < 0) {
      return r;
    }

    cls::rbd::MirrorImageStatus
      s(cls::rbd::MIRROR_IMAGE_STATUS_STATE_UNKNOWN, "status not found");

    r = cls_client::mirror_image_status_get(&ictx->md_ctx, info.global_id, &s);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "failed to retrieve image mirror status: "
		 << cpp_strerror(r) << dendl;
      return r;
    }

    *status = mirror_image_status_t{
      ictx->name,
      info,
      static_cast<mirror_image_status_state_t>(s.state),
      s.description,
      s.last_update.sec(),
      s.up};
    return 0;
  }

  int mirror_mode_get(IoCtx& io_ctx, rbd_mirror_mode_t *mirror_mode) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << dendl;

    cls::rbd::MirrorMode mirror_mode_internal;
    int r = cls_client::mirror_mode_get(&io_ctx, &mirror_mode_internal);
    if (r < 0) {
      lderr(cct) << "Failed to retrieve mirror mode: " << cpp_strerror(r)
                 << dendl;
      return r;
    }

    switch (mirror_mode_internal) {
    case cls::rbd::MIRROR_MODE_DISABLED:
    case cls::rbd::MIRROR_MODE_IMAGE:
    case cls::rbd::MIRROR_MODE_POOL:
      *mirror_mode = static_cast<rbd_mirror_mode_t>(mirror_mode_internal);
      break;
    default:
      lderr(cct) << "Unknown mirror mode ("
                 << static_cast<uint32_t>(mirror_mode_internal) << ")"
                 << dendl;
      return -EINVAL;
    }
    return 0;
  }

  int list_mirror_images(IoCtx& io_ctx,
                         std::set<std::string>& mirror_image_ids) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());

    std::string last_read = "";
    int max_read = 1024;
    int r;
    do {
      std::map<std::string, std::string> mirror_images;
      r =  cls_client::mirror_image_list(&io_ctx, last_read, max_read,
                                             &mirror_images);
      if (r < 0) {
        lderr(cct) << "error listing mirrored image directory: "
             << cpp_strerror(r) << dendl;
        return r;
      }
      for (auto it = mirror_images.begin(); it != mirror_images.end(); ++it) {
        mirror_image_ids.insert(it->first);
      }
      if (!mirror_images.empty()) {
        last_read = mirror_images.rbegin()->first;
      }
      r = mirror_images.size();
    } while (r == max_read);

    return 0;
  }

  int mirror_mode_set(IoCtx& io_ctx, rbd_mirror_mode_t mirror_mode) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << dendl;

    cls::rbd::MirrorMode next_mirror_mode;
    switch (mirror_mode) {
    case RBD_MIRROR_MODE_DISABLED:
    case RBD_MIRROR_MODE_IMAGE:
    case RBD_MIRROR_MODE_POOL:
      next_mirror_mode = static_cast<cls::rbd::MirrorMode>(mirror_mode);
      break;
    default:
      lderr(cct) << "Unknown mirror mode ("
                 << static_cast<uint32_t>(mirror_mode) << ")" << dendl;
      return -EINVAL;
    }

    int r;
    if (next_mirror_mode == cls::rbd::MIRROR_MODE_DISABLED) {
      // fail early if pool still has peers registered and attempting to disable
      std::vector<cls::rbd::MirrorPeer> mirror_peers;
      r = cls_client::mirror_peer_list(&io_ctx, &mirror_peers);
      if (r < 0 && r != -ENOENT) {
        lderr(cct) << "Failed to list peers: " << cpp_strerror(r) << dendl;
        return r;
      } else if (!mirror_peers.empty()) {
        lderr(cct) << "mirror peers still registered" << dendl;
        return -EBUSY;
      }
    }

    cls::rbd::MirrorMode current_mirror_mode;
    r = cls_client::mirror_mode_get(&io_ctx, &current_mirror_mode);
    if (r < 0) {
      lderr(cct) << "Failed to retrieve mirror mode: " << cpp_strerror(r)
                 << dendl;
      return r;
    }

    if (current_mirror_mode == next_mirror_mode) {
      return 0;
    } else if (current_mirror_mode == cls::rbd::MIRROR_MODE_DISABLED) {
      uuid_d uuid_gen;
      uuid_gen.generate_random();
      r = cls_client::mirror_uuid_set(&io_ctx, uuid_gen.to_string());
      if (r < 0) {
        lderr(cct) << "Failed to allocate mirroring uuid: " << cpp_strerror(r)
                   << dendl;
        return r;
      }
    }

    if (current_mirror_mode != cls::rbd::MIRROR_MODE_IMAGE) {
      r = cls_client::mirror_mode_set(&io_ctx, cls::rbd::MIRROR_MODE_IMAGE);
      if (r < 0) {
        lderr(cct) << "failed to set mirror mode to image: "
                   << cpp_strerror(r) << dendl;
        return r;
      }

      r = MirroringWatcher<>::notify_mode_updated(io_ctx,
                                                  cls::rbd::MIRROR_MODE_IMAGE);
      if (r < 0) {
        lderr(cct) << "failed to send update notification: " << cpp_strerror(r)
                   << dendl;
      }
    }

    if (next_mirror_mode == cls::rbd::MIRROR_MODE_IMAGE) {
      return 0;
    }

    if (next_mirror_mode == cls::rbd::MIRROR_MODE_POOL) {
      map<string, string> images;
      r = list_images_v2(io_ctx, images);
      if (r < 0) {
        lderr(cct) << "Failed listing images: " << cpp_strerror(r) << dendl;
        return r;
      }

      for (const auto& img_pair : images) {
        uint64_t features;
        r = cls_client::get_features(&io_ctx,
                                     util::header_name(img_pair.second),
                                     CEPH_NOSNAP, &features);
        if (r < 0) {
          lderr(cct) << "error getting features for image " << img_pair.first
                     << ": " << cpp_strerror(r) << dendl;
          return r;
        }

        if ((features & RBD_FEATURE_JOURNALING) != 0) {
          ImageCtx *img_ctx = new ImageCtx("", img_pair.second, nullptr,
                                           io_ctx, false);
          r = img_ctx->state->open(false);
          if (r < 0) {
            lderr(cct) << "error opening image "<< img_pair.first << ": "
                       << cpp_strerror(r) << dendl;
            delete img_ctx;
            return r;
          }

          r = mirror_image_enable(img_ctx, true);
          if (r < 0) {
            lderr(cct) << "error enabling mirroring for image "
                       << img_pair.first << ": " << cpp_strerror(r) << dendl;
            return r;
          }

          r = img_ctx->state->close();
          if (r < 0) {
            lderr(cct) << "failed to close image " << img_pair.first << ": "
                       << cpp_strerror(r) << dendl;
            return r;
          }
        }
      }
    } else if (next_mirror_mode == cls::rbd::MIRROR_MODE_DISABLED) {
      std::set<std::string> image_ids;
      r = list_mirror_images(io_ctx, image_ids);
      if (r < 0) {
        lderr(cct) << "Failed listing images: " << cpp_strerror(r) << dendl;
        return r;
      }

      for (const auto& img_id : image_ids) {
        if (current_mirror_mode == cls::rbd::MIRROR_MODE_IMAGE) {
          cls::rbd::MirrorImage mirror_image;
          r = cls_client::mirror_image_get(&io_ctx, img_id, &mirror_image);
          if (r < 0 && r != -ENOENT) {
            lderr(cct) << "failed to retrieve mirroring state for image id "
                       << img_id << ": " << cpp_strerror(r) << dendl;
            return r;
          }
          if (mirror_image.state == cls::rbd::MIRROR_IMAGE_STATE_ENABLED) {
            lderr(cct) << "Failed to disable mirror mode: there are still "
                       << "images with mirroring enabled" << dendl;
            return -EINVAL;
          }
        } else {
          ImageCtx *img_ctx = new ImageCtx("", img_id, nullptr, io_ctx, false);
          r = img_ctx->state->open(false);
          if (r < 0) {
            lderr(cct) << "error opening image id "<< img_id << ": "
                       << cpp_strerror(r) << dendl;
            delete img_ctx;
            return r;
          }

          r = mirror_image_disable(img_ctx, false);
          if (r < 0) {
            lderr(cct) << "error disabling mirroring for image id " << img_id
                       << cpp_strerror(r) << dendl;
            return r;
          }

          r = img_ctx->state->close();
          if (r < 0) {
            lderr(cct) << "failed to close image id " << img_id << ": "
                       << cpp_strerror(r) << dendl;
            return r;
          }
        }
      }
    }

    r = cls_client::mirror_mode_set(&io_ctx, next_mirror_mode);
    if (r < 0) {
      lderr(cct) << "Failed to set mirror mode: " << cpp_strerror(r) << dendl;
      return r;
    }

    r = MirroringWatcher<>::notify_mode_updated(io_ctx, next_mirror_mode);
    if (r < 0) {
      lderr(cct) << "failed to send update notification: " << cpp_strerror(r)
                 << dendl;
    }
    return 0;
  }

  int mirror_peer_add(IoCtx& io_ctx, std::string *uuid,
                      const std::string &cluster_name,
                      const std::string &client_name) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << ": "
                   << "name=" << cluster_name << ", "
                   << "client=" << client_name << dendl;

    if (cct->_conf->cluster == cluster_name) {
      lderr(cct) << "Cannot add self as remote peer" << dendl;
      return -EINVAL;
    }

    int r;
    do {
      uuid_d uuid_gen;
      uuid_gen.generate_random();

      *uuid = uuid_gen.to_string();
      r = cls_client::mirror_peer_add(&io_ctx, *uuid, cluster_name,
                                      client_name);
      if (r == -ESTALE) {
        ldout(cct, 5) << "Duplicate UUID detected, retrying" << dendl;
      } else if (r < 0) {
        lderr(cct) << "Failed to add mirror peer '" << uuid << "': "
                   << cpp_strerror(r) << dendl;
        return r;
      }
    } while (r == -ESTALE);
    return 0;
  }

  int mirror_peer_remove(IoCtx& io_ctx, const std::string &uuid) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << ": uuid=" << uuid << dendl;

    int r = cls_client::mirror_peer_remove(&io_ctx, uuid);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "Failed to remove peer '" << uuid << "': "
                 << cpp_strerror(r) << dendl;
      return r;
    }
    return 0;
  }

  int mirror_peer_list(IoCtx& io_ctx, std::vector<mirror_peer_t> *peers) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << dendl;

    std::vector<cls::rbd::MirrorPeer> mirror_peers;
    int r = cls_client::mirror_peer_list(&io_ctx, &mirror_peers);
    if (r < 0 && r != -ENOENT) {
      lderr(cct) << "Failed to list peers: " << cpp_strerror(r) << dendl;
      return r;
    }

    peers->clear();
    peers->reserve(mirror_peers.size());
    for (auto &mirror_peer : mirror_peers) {
      mirror_peer_t peer;
      peer.uuid = mirror_peer.uuid;
      peer.cluster_name = mirror_peer.cluster_name;
      peer.client_name = mirror_peer.client_name;
      peers->push_back(peer);
    }
    return 0;
  }

  int mirror_peer_set_client(IoCtx& io_ctx, const std::string &uuid,
                             const std::string &client_name) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << ": uuid=" << uuid << ", "
                   << "client=" << client_name << dendl;

    int r = cls_client::mirror_peer_set_client(&io_ctx, uuid, client_name);
    if (r < 0) {
      lderr(cct) << "Failed to update client '" << uuid << "': "
                 << cpp_strerror(r) << dendl;
      return r;
    }
    return 0;
  }

  int mirror_peer_set_cluster(IoCtx& io_ctx, const std::string &uuid,
                              const std::string &cluster_name) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    ldout(cct, 20) << __func__ << ": uuid=" << uuid << ", "
                   << "cluster=" << cluster_name << dendl;

    int r = cls_client::mirror_peer_set_cluster(&io_ctx, uuid, cluster_name);
    if (r < 0) {
      lderr(cct) << "Failed to update cluster '" << uuid << "': "
                 << cpp_strerror(r) << dendl;
      return r;
    }
    return 0;
  }

  int mirror_image_status_list(IoCtx& io_ctx, const std::string &start_id,
      size_t max, std::map<std::string, mirror_image_status_t> *images) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
    int r;

    map<string, string> id_to_name;
    {
      map<string, string> name_to_id;
      r = list_images_v2(io_ctx, name_to_id);
      if (r < 0) {
	return r;
      }
      for (auto it : name_to_id) {
	id_to_name[it.second] = it.first;
      }
    }

    map<std::string, cls::rbd::MirrorImage> images_;
    map<std::string, cls::rbd::MirrorImageStatus> statuses_;

    r = librbd::cls_client::mirror_image_status_list(&io_ctx, start_id, max,
						     &images_, &statuses_);
    if (r < 0) {
      lderr(cct) << "Failed to list mirror image statuses: "
                 << cpp_strerror(r) << dendl;
      return r;
    }

    cls::rbd::MirrorImageStatus unknown_status(
      cls::rbd::MIRROR_IMAGE_STATUS_STATE_UNKNOWN, "status not found");

    for (auto it = images_.begin(); it != images_.end(); ++it) {
      auto &image_id = it->first;
      auto &info = it->second;
      auto &image_name = id_to_name[image_id];
      if (image_name.empty()) {
	lderr(cct) << "Failed to find image name for image " << image_id
		   << ", using image id as name" << dendl;
	image_name = image_id;
      }
      auto s_it = statuses_.find(image_id);
      auto &s = s_it != statuses_.end() ? s_it->second : unknown_status;
      (*images)[image_id] = mirror_image_status_t{
	image_name,
	mirror_image_info_t{
	  info.global_image_id,
	  static_cast<mirror_image_state_t>(info.state),
	  false}, // XXX: To set "primary" right would require an additional call.
	static_cast<mirror_image_status_state_t>(s.state),
	s.description,
	s.last_update.sec(),
	s.up};
    }

    return 0;
  }

  int mirror_image_status_summary(IoCtx& io_ctx,
    std::map<mirror_image_status_state_t, int> *states) {
    CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());

    std::map<cls::rbd::MirrorImageStatusState, int> states_;
    int r = cls_client::mirror_image_status_get_summary(&io_ctx, &states_);
    if (r < 0) {
      lderr(cct) << "Failed to get mirror status summary: "
                 << cpp_strerror(r) << dendl;
      return r;
    }
    for (auto &s : states_) {
      (*states)[static_cast<mirror_image_status_state_t>(s.first)] = s.second;
    }
    return 0;
  }

  struct C_RBD_Readahead : public Context {
    ImageCtx *ictx;
    object_t oid;
    uint64_t offset;
    uint64_t length;
    C_RBD_Readahead(ImageCtx *ictx, object_t oid, uint64_t offset, uint64_t length)
      : ictx(ictx), oid(oid), offset(offset), length(length) { }
    void finish(int r) {
      ldout(ictx->cct, 20) << "C_RBD_Readahead on " << oid << ": " << offset << "+" << length << dendl;
      ictx->readahead.dec_pending();
    }
  };

  void readahead(ImageCtx *ictx,
                 const vector<pair<uint64_t,uint64_t> >& image_extents)
  {
    uint64_t total_bytes = 0;
    for (vector<pair<uint64_t,uint64_t> >::const_iterator p = image_extents.begin();
	 p != image_extents.end();
	 ++p) {
      total_bytes += p->second;
    }
    
    ictx->md_lock.get_write();
    bool abort = ictx->readahead_disable_after_bytes != 0 &&
      ictx->total_bytes_read > ictx->readahead_disable_after_bytes;
    if (abort) {
      ictx->md_lock.put_write();
      return;
    }
    ictx->total_bytes_read += total_bytes;
    ictx->snap_lock.get_read();
    uint64_t image_size = ictx->get_image_size(ictx->snap_id);
    ictx->snap_lock.put_read();
    ictx->md_lock.put_write();
    
    pair<uint64_t, uint64_t> readahead_extent = ictx->readahead.update(image_extents, image_size);
    uint64_t readahead_offset = readahead_extent.first;
    uint64_t readahead_length = readahead_extent.second;

    if (readahead_length > 0) {
      ldout(ictx->cct, 20) << "(readahead logical) " << readahead_offset << "~" << readahead_length << dendl;
      map<object_t,vector<ObjectExtent> > readahead_object_extents;
      Striper::file_to_extents(ictx->cct, ictx->format_string, &ictx->layout,
			       readahead_offset, readahead_length, 0, readahead_object_extents);
      for (map<object_t,vector<ObjectExtent> >::iterator p = readahead_object_extents.begin(); p != readahead_object_extents.end(); ++p) {
	for (vector<ObjectExtent>::iterator q = p->second.begin(); q != p->second.end(); ++q) {
	  ldout(ictx->cct, 20) << "(readahead) oid " << q->oid << " " << q->offset << "~" << q->length << dendl;

	  Context *req_comp = new C_RBD_Readahead(ictx, q->oid, q->offset, q->length);
	  ictx->readahead.inc_pending();
	  ictx->aio_read_from_cache(q->oid, q->objectno, NULL,
				    q->length, q->offset,
				    req_comp, 0);
	}
      }
      ictx->perfcounter->inc(l_librbd_readahead);
      ictx->perfcounter->inc(l_librbd_readahead_bytes, readahead_length);
    }
  }



}
