#include "rkisp_control_loop_impl.h"
#include "rkisp_dev_manager.h"

#include <isp_poll_thread.h>
#include <isp_image_processor.h>
#include "iq/x3a_analyze_tuner.h"
#include "x3a_analyzer_rkiq.h"
#include "dynamic_analyzer_loader.h"

#include "mediactl-priv.h"
#include "mediactl.h"
#include "v4l2subdev.h"

#include <base/log.h>

#define V4L2_CAPTURE_MODE_STILL 0x2000
#define V4L2_CAPTURE_MODE_VIDEO 0x4000
#define V4L2_CAPTURE_MODE_PREVIEW 0x8000

#define MAX_MEDIA_INDEX 16

#define AIQ_CONTEXT_CAST(context)  ((RkispDeviceManager*)(context))

using namespace XCam;
using namespace android;

CameraMetadata RkispDeviceManager::staticMeta;

typedef enum RKISP_CL_STATE_enum {
    RKISP_CL_STATE_INVALID  = -1,
    RKISP_CL_STATE_INITED   =  0,
    RKISP_CL_STATE_PREPARED     ,
    RKISP_CL_STATE_STARTED      ,
    RKISP_CL_STATE_PAUSED       ,
} RKISP_CL_STATE_e;

int rkisp_cl_init(void** cl_ctx, const char* tuning_file_path,
                  const cl_result_callback_ops_t *callback_ops) {
    LOGD("--------------------------rkisp_cl_init");
    RkispDeviceManager *device_manager = new RkispDeviceManager(callback_ops);
    if (tuning_file_path && access(tuning_file_path, F_OK) == 0) {
        device_manager->set_iq_path(tuning_file_path);
        device_manager->set_has_3a(true);
        LOGD("Enable 3a, using IQ file path %s", tuning_file_path);
    } else {
        device_manager->set_has_3a(false);
        LOGD("Disable 3a, don't find IQ file");
    }
    device_manager->_cl_state = RKISP_CL_STATE_INITED;
    *cl_ctx = (void*)device_manager;
    return 0;
}

static int __rkisp_get_isp_ver(V4l2Device* vdev, int* isp_ver) {
    struct v4l2_capability cap;

    if (vdev->query_cap(cap) == XCAM_RETURN_NO_ERROR) {
        char *p;
        p = strrchr((char*)cap.driver, '_');
        if (p == NULL)
            goto out;
        // version info:
        // rk3399,rk3288: 0
        // rk3326:        2
        // rk1808:        3
        if (*(p + 1) != 'v')
            goto out;
        *isp_ver = atoi(p + 2);

        return 0;
    }

out:
    LOGE("get isp version failed !");
    return -1;
}

static int __rkisp_get_sensor_name(const char* vnode, char* sensor_name) {
    char sys_path[64];
    struct media_device *device = NULL;
    uint32_t nents, j, i = 0;
    FILE *fp;
    int ret;

    while (i < MAX_MEDIA_INDEX) {
        snprintf (sys_path, 64, "/dev/media%d", i++);
        fp = fopen (sys_path, "r");
        if (!fp)
          continue;
        fclose (fp);

        device = media_device_new (sys_path);

        /* Enumerate entities, pads and links. */
        media_device_enumerate (device);

        nents = media_get_entities_count (device);
        for (j = 0; j < nents; ++j) {
          struct media_entity *entity = media_get_entity (device, j);
          const char *devname = media_entity_get_devname (entity);
          if (NULL != devname) {
            // get dev path
            char devpath[32];
            char sysname[32];
            char target[1024];
            char *p;

            sprintf(sysname, "/sys/dev/char/%u:%u", entity->info.v4l.major,
                entity->info.v4l.minor);
            ret = readlink(sysname, target, sizeof(target));
            if (ret < 0)
                return -errno;

            target[ret] = '\0';
            p = strrchr(target, '/');
            if (p == NULL)
              continue ;
            sprintf(devpath, "/dev/%s", p + 1);

            LOGD("entity name %s", entity->info.name);
            if (!strcmp (devpath, vnode)) {
              strcpy(sensor_name, entity->info.name);
              media_device_unref (device);
              return 0;
            }
          }
        }
        media_device_unref (device);
    }

    return -1;
}

int rkisp_cl_prepare(void* cl_ctx,
                     const struct rkisp_cl_prepare_params_s* prepare_params) {
	LOGD("--------------------------rkisp_cl_prepare");

    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    RkispDeviceManager *device_manager = AIQ_CONTEXT_CAST (cl_ctx);
    SmartPtr<V4l2SubDevice> isp_dev = NULL;
    SmartPtr<V4l2SubDevice> sensor_dev = NULL;
    SmartPtr<V4l2SubDevice> vcm_dev = NULL;
    SmartPtr<V4l2Device> stats_dev = NULL;
    SmartPtr<V4l2Device> param_dev = NULL;

    if (device_manager->_cl_state == RKISP_CL_STATE_INVALID) {
        LOGE("%s: cl haven't been init %d", __FUNCTION__, device_manager->_cl_state);
        return -1;
    }
    if (device_manager->_cl_state >= RKISP_CL_STATE_PREPARED) {
        LOGI("%s: cl has already been prepared, now in state %d",
             __FUNCTION__, device_manager->_cl_state);
        return 0;
    }
	LOGD("rkisp_cl_prepare, isp: %s, sensor: %s, stats: %s, params: %s, lens: %s",
        prepare_params->isp_sd_node_path,
        prepare_params->sensor_sd_node_path,
        prepare_params->isp_vd_stats_path,
        prepare_params->isp_vd_params_path,
        prepare_params->lens_sd_node_path);

    isp_dev = new V4l2SubDevice (prepare_params->isp_sd_node_path);
    ret = isp_dev->open ();
    if (ret == XCAM_RETURN_NO_ERROR) {
        isp_dev->subscribe_event (V4L2_EVENT_FRAME_SYNC);
        device_manager->set_event_subdevice(isp_dev);
    } else {
        ALOGE("failed to open isp subdev");
        return -1;
    }

    sensor_dev = new V4l2SubDevice (prepare_params->sensor_sd_node_path);
    ret = sensor_dev->open ();
    if (ret == XCAM_RETURN_NO_ERROR) {
        //sensor_dev->subscribe_event (V4L2_EVENT_FRAME_SYNC);
        char sensor_name[32];
        if (__rkisp_get_sensor_name(prepare_params->sensor_sd_node_path, sensor_name))
            LOGW("%s: can't get sensor name");
        device_manager->set_sensor_subdevice(sensor_dev, sensor_name);
    } else {
        ALOGE("failed to open isp subdev");
        return -1;
    }

    stats_dev = new V4l2Device (prepare_params->isp_vd_stats_path);
    stats_dev->set_sensor_id (0);
    stats_dev->set_capture_mode (V4L2_CAPTURE_MODE_VIDEO);
    stats_dev->set_buf_type(V4L2_BUF_TYPE_META_CAPTURE);
    stats_dev->set_mem_type (V4L2_MEMORY_MMAP);
    stats_dev->set_buffer_count (1);
    ret = stats_dev->open ();
    if (ret == XCAM_RETURN_NO_ERROR) {
        device_manager->set_isp_stats_device (stats_dev);
    } else {
        ALOGE("failed to open statistics dev");
        return -1;
    }

    int isp_ver = 0;
    if (__rkisp_get_isp_ver(stats_dev.ptr(), &isp_ver))
        LOGW("get isp version failed, please check ISP driver !");
    LOGD("isp version is %d !", isp_ver);
    device_manager->set_isp_ver(isp_ver);
    param_dev = new V4l2Device (prepare_params->isp_vd_params_path);
    param_dev->set_sensor_id (0);
    param_dev->set_capture_mode (V4L2_CAPTURE_MODE_VIDEO);
    param_dev->set_buf_type(V4L2_BUF_TYPE_META_OUTPUT);
    param_dev->set_mem_type (V4L2_MEMORY_MMAP);
    param_dev->set_buffer_count (1);
    ret = param_dev->open ();
    if (ret == XCAM_RETURN_NO_ERROR) {
        device_manager->set_isp_params_device (param_dev);
    } else {
        ALOGE("failed to open parameter dev");
        return -1;
    }

    if (prepare_params->lens_sd_node_path) {
        vcm_dev = new V4l2SubDevice(prepare_params->lens_sd_node_path);
        ret = vcm_dev->open ();
        if (ret != XCAM_RETURN_NO_ERROR) {
            ALOGE("failed to open isp subdev");
            return -1;
        }
    }

    SmartPtr<IspController> isp_controller = new IspController ();
    isp_controller->set_sensor_subdev(sensor_dev);
    isp_controller->set_isp_stats_device(stats_dev);
    isp_controller->set_isp_params_device(param_dev);
    isp_controller->set_isp_ver(isp_ver);
    if (vcm_dev.ptr())
        isp_controller->set_vcm_subdev(vcm_dev);

    SmartPtr<IspPollThread> isp_poll_thread = new IspPollThread ();
    isp_poll_thread->set_isp_controller (isp_controller);
    device_manager->set_poll_thread (isp_poll_thread);

    SmartPtr<ImageProcessor> isp_processor = new IspImageProcessor (isp_controller, true);
    device_manager->add_image_processor (isp_processor);

    SmartPtr<X3aAnalyzer> aiq_analyzer =
        new X3aAnalyzerRKiq (device_manager, isp_controller, device_manager->get_iq_path());
    device_manager->set_3a_analyzer (aiq_analyzer);

    device_manager->set_static_metadata (prepare_params->staticMeta);

    device_manager->_cl_state = RKISP_CL_STATE_PREPARED;
    LOGD("--------------------------rkisp_cl_prepare done");

    return 0;
}

int rkisp_cl_start(void* cl_ctx) {
    int ret = 0;
    LOGD("--------------------------rkisp_cl_start");
    RkispDeviceManager *device_manager = AIQ_CONTEXT_CAST (cl_ctx);

    if (device_manager->_cl_state < RKISP_CL_STATE_PREPARED) {
        LOGE("%s: invalid cl state %d", __FUNCTION__, device_manager->_cl_state);
        return -1;
    }
    if (device_manager->_cl_state == RKISP_CL_STATE_PAUSED) {
        device_manager->resume_dequeue ();
    } else {
        ret = device_manager->start();
        if (ret != XCAM_RETURN_NO_ERROR) {
            device_manager->stop();
            device_manager->pause_dequeue ();
        }
    }
    device_manager->_cl_state = RKISP_CL_STATE_STARTED;
    LOGD("--------------------------rkisp_cl_start done");

    return ret;
}

int rkisp_cl_set_frame_params(const void* cl_ctx,
                              const struct rkisp_cl_frame_metadata_s* frame_params) {
    int ret = 0;
    LOGD("--------------------------rkisp_cl_set_frame_params");
    RkispDeviceManager *device_manager = AIQ_CONTEXT_CAST (cl_ctx);

    ret = device_manager->set_control_params(frame_params->id, frame_params->metas);
    if (ret != XCAM_RETURN_NO_ERROR) {
        LOGE("@%s %d: set_control_params failed ", __FUNCTION__, __LINE__);
    }
    return 0;
}

// implement interface stop as pause so we could keep all the 3a status,
// and could speed up 3a converged
int rkisp_cl_stop(void* cl_ctx) {
    RkispDeviceManager *device_manager = AIQ_CONTEXT_CAST (cl_ctx);
    LOGD("--------------------------rkisp_cl_stop");
    //device_manager->stop();
    device_manager->pause_dequeue ();
    device_manager->_cl_state = RKISP_CL_STATE_PAUSED;
    LOGD("--------------------------rkisp_cl_stop done");
    return 0;
}

void rkisp_cl_deinit(void* cl_ctx) {
    LOGD("--------------------------rkisp_cl_deinit");
    RkispDeviceManager *device_manager = AIQ_CONTEXT_CAST (cl_ctx);
    if (device_manager->is_running ()) {
        device_manager->stop();
        device_manager->pause_dequeue ();
    }
    device_manager->_cl_state = RKISP_CL_STATE_INVALID;
    delete device_manager;
    device_manager = NULL;
    LOGD("--------------------------rkisp_cl_deinit done");
}

