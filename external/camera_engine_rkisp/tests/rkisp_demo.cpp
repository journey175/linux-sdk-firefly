/*
 * V4L2 video capture example
 * AUTHOT : Jacob Chen
 * DATA : 2018-02-25
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h> /* getopt_long() */
#include <fcntl.h> /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <dlfcn.h>

#include <linux/videodev2.h>
#include <rkisp_control_loop.h>
#include <rkisp_dev_manager.h>
#include "mediactl.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define FMT_NUM_PLANES 1

void* _rkisp_engine;

typedef int (*rkisp_init_func)(void** cl_ctx, const char* tuning_file_path,
                               const cl_result_callback_ops_t *callback_ops);
typedef int (*rkisp_prepare_func)(void* cl_ctx,
                     const struct rkisp_cl_prepare_params_s* prepare_params);
typedef int (*rkisp_start_func)(void* cl_ctx);
typedef int (*rkisp_stop_func)(void* cl_ctx);
typedef int (*rkisp_deinit_func)(void* cl_ctx);
typedef int (*rkisp_cl_set_frame_params_func)(const void* cl_ctx,
                     const struct rkisp_cl_frame_metadata_s* frame_params);

struct RKIspFunc {
    void* rkisp_handle;
    rkisp_init_func init_func;
    rkisp_prepare_func prepare_func;
    rkisp_start_func start_func;
    rkisp_stop_func stop_func;
    rkisp_deinit_func deinit_func;
    rkisp_cl_set_frame_params_func set_frame_params_func;
};
struct RKIspFunc _RKIspFunc;

struct RKisp_media_ctl
{
    /* media controller */
    media_device *controller;
    media_entity *isp_subdev;
    media_entity *isp_params_dev;
    media_entity *isp_stats_dev;
    media_entity *sensor_subdev;
};
struct buffer {
        void *start;
        size_t length;
};
static char iq_file[255] = "/etc/cam_iq.xml";
static char out_file[255];
static char dev_name[255];
static int width = 640;
static int height = 480;
static int format = V4L2_PIX_FMT_NV12;
static int fd = -1;
static enum v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
struct buffer *buffers;
static unsigned int n_buffers;
static int frame_count = 5;
FILE *fp;
#ifdef ANDROID
#ifdef ANDROID_VERSION_ABOVE_8_X
#define LIBRKISP "/vendor/lib64/librkisp.so"
#else
#define LIBRKISP "/system/lib/librkisp.so"
#endif
#else
#define LIBRKISP "/usr/lib/librkisp.so"
#endif

struct control_params_3A
{
    /* used to receive current 3A settings and 3A states
     * place this at first place, so we cast ops back to base type
     */
    cl_result_callback_ops_t _result_cb_ops;
    /* used to set new setting to CL, used by _RKIspFunc.set_frame_params_func */
    rkisp_cl_frame_metadata_s _frame_metas;
    /* to manage the 3A settings, used by _frame_metas */
    CameraMetadata _settings_metadata;
    /* to manage the 3A result settings, used by metadata_result_callback */
    CameraMetadata _result_metadata;
    XCam::Mutex _meta_mutex;
};

static struct control_params_3A* g_3A_control_params = NULL;

static
void metadata_result_callback(const struct cl_result_callback_ops *ops,
                              struct rkisp_cl_frame_metadata_s *result)
{
    camera_metadata_entry entry;
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)ops;

    SmartLock lock(ctl_params->_meta_mutex);
    /* this will clone results to _result_metadata */
    ctl_params->_result_metadata = result->metas;
    printf("meta callback!\n");
}

/*
 * construct the default camera settings, including the static
 * camera capabilities, infos.
 */
static void construct_default_metas(CameraMetadata* metas)
{
    int64_t exptime_range_ns[2] = {0,30*1000*1000};
    int32_t sensitivity_range[2] = {0,3200};
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_ON;
    uint8_t control_mode = ANDROID_CONTROL_MODE_AUTO;
    uint8_t ae_lock = ANDROID_CONTROL_AE_LOCK_OFF;
    int64_t exptime_ns = 10*1000*1000;
    int32_t sensitivity = 1600;

    metas->update(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exptime_range_ns, 2);
    metas->update(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);
    metas->update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    metas->update(ANDROID_SENSOR_EXPOSURE_TIME, &exptime_ns, 1);
    metas->update(ANDROID_CONTROL_MODE, &control_mode, 1);
    metas->update(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    metas->update(ANDROID_CONTROL_AE_LOCK, &ae_lock, 1);
}

static void init_3A_control_params()
{
    camera_metadata_t* meta;

    meta = allocate_camera_metadata(DEFAULT_ENTRY_CAP, DEFAULT_DATA_CAP);
    assert(meta);
    g_3A_control_params = new control_params_3A();
    assert(g_3A_control_params);
    g_3A_control_params->_result_cb_ops.metadata_result_callback = metadata_result_callback;
    g_3A_control_params->_settings_metadata = meta;
    construct_default_metas(&g_3A_control_params->_settings_metadata);
    g_3A_control_params->_frame_metas.id = 0;
    g_3A_control_params->_frame_metas.metas =
        g_3A_control_params->_settings_metadata.getAndLock();
    g_3A_control_params->_settings_metadata.unlock(g_3A_control_params->_frame_metas.metas);
}

static void deinit_3A_control_params()
{
    if (g_3A_control_params )
        delete g_3A_control_params ;
    g_3A_control_params = NULL;
}

// convenient interfaces of 3A, compatible with cifisp
static int rkisp_getAeTime(void* &engine, float &time)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_EXPOSURE_TIME);
    if (!entry.count)
        return -1;

    time = entry.data.i64[0] / (1000.0 * 1000.0 * 1000.0);
    printf("expousre time is %f secs\n", time);

    return 0;
}

static int rkisp_getAeMaxExposureTime(void* &engine, float &time)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE);
    if (entry.count != 2)
        return -1;

    time = entry.data.i64[1] / (1000.0 * 1000.0 * 1000.0);
    printf("expousre max time is %f secs\n", time);

    return 0;
}

static int rkisp_getAeGain(void* &engine, float &gain)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_SENSITIVITY);
    if (!entry.count)
        return -1;

    gain = (float)entry.data.i32[0] / 100;
    printf("expousre gain is %f\n", gain);

    return 0;
}

static int rkisp_getAeMaxExposureGain(void* &engine, float &gain)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE);
    if (!entry.count)
        return -1;

    gain = entry.data.i32[1] / 100;
    printf("expousre max gain is %f \n", gain);

    return 0;
}

static int rkisp_setAeMaxExposureTime(void* &engine, float time)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;

    int64_t exptime_range_ns[2] = {0,0};

    exptime_range_ns[1] = time * 1000 * 1000 * 1000;
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                                          exptime_range_ns, 2);
    // should update new settings id
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);
    return 0;
}

static int rkisp_setAeMaxExposureGain(void* &engine, float gain)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;

    int32_t sensitivity_range[2] = {0,0};

    sensitivity_range[1] = gain * 100;
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
                                          sensitivity_range, 2);
    // should update new settings id
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);
    return 0;
}

static int rkisp_setManualGainAndTime(void* &engine, float hal_gain, float hal_time)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;

    int64_t exptime_ns = hal_time * 1000 * 1000 * 1000;
    // set to manual mode
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_OFF;
    // convert to ISO100 unit
    int32_t sensitivity = hal_gain * 100;

    ctl_params->_settings_metadata.update(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    ctl_params->_settings_metadata.update(ANDROID_SENSOR_EXPOSURE_TIME, &exptime_ns, 1);
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);

    return 0;
}

static int rkisp_setAeMode(void* &engine, HAL_AE_OPERATION_MODE mode)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    // set to manual mode
    uint8_t ae_mode = ANDROID_CONTROL_AE_MODE_OFF;

    if (mode == HAL_AE_OPERATION_MODE_AUTO) {
        ae_mode = ANDROID_CONTROL_AE_MODE_ON;
        ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    } else if (mode == HAL_AE_OPERATION_MODE_MANUAL) {
        camera_metadata_entry entry;
        SmartLock lock(ctl_params->_meta_mutex);

        entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_EXPOSURE_TIME);
        if (!entry.count)
            return -1;
        ctl_params->_settings_metadata.update(ANDROID_SENSOR_EXPOSURE_TIME, entry.data.i64, 1);

        entry = ctl_params->_result_metadata.find(ANDROID_SENSOR_SENSITIVITY);
        if (!entry.count)
            return -1;
        ctl_params->_settings_metadata.update(ANDROID_SENSOR_SENSITIVITY, entry.data.i32, 1);

        ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_MODE, &ae_mode, 1);
    } else {
        printf("unsupported ae mode %d", mode);
        return -1;
    }
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);

    return 0;
}

enum HAL_AE_STATE {
    HAL_AE_STATE_UNSTABLE,
    HAL_AE_STATE_STABLE,
};

static int rkisp_getAeState(void* &engine, enum HAL_AE_STATE &ae_state)
{
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_CONTROL_AE_STATE);
    if (!entry.count)
        return -1;

    switch (entry.data.u8[0]) {
    case ANDROID_CONTROL_AE_STATE_SEARCHING :
        ae_state = HAL_AE_STATE_UNSTABLE;
        break;
    default:
        ae_state = HAL_AE_STATE_STABLE;
        break;
    }

    return 0;
}

static int rkisp_set3ALocks(void* &engine, int locks)
{
    // TODO: only support ae lock now
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;

    uint8_t ae_lock = locks & HAL_3A_LOCKS_EXPOSURE ?
        ANDROID_CONTROL_AE_LOCK_ON : ANDROID_CONTROL_AE_LOCK_OFF;

    ctl_params->_settings_metadata.update(ANDROID_CONTROL_AE_LOCK, &ae_lock, 1);
    ctl_params->_frame_metas.id++;
    ctl_params->_frame_metas.metas =
        ctl_params->_settings_metadata.getAndLock();
    ctl_params->_settings_metadata.unlock(ctl_params->_frame_metas.metas);

    _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                     &ctl_params->_frame_metas);
    return 0;
}

static int rkisp_get3ALocks(void* &engine, int& curLocks)
{
    // TODO: only support ae lock now
    struct control_params_3A* ctl_params =
        (struct control_params_3A*)engine;
    camera_metadata_entry entry;

    SmartLock lock(ctl_params->_meta_mutex);

    entry = ctl_params->_result_metadata.find(ANDROID_CONTROL_AE_LOCK);
    if (!entry.count)
        return -1;

    if (entry.data.u8[0] == ANDROID_CONTROL_AE_LOCK_ON)
        curLocks |= HAL_3A_LOCKS_EXPOSURE;

    return 0;
}

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
        int r;
        do {
                r = ioctl(fh, request, arg);
        } while (-1 == r && EINTR == errno);
        return r;
}

static void process_image(const void *p, int size)
{
		printf("process_image size: %d\n",size);
        fwrite(p,size, 1, fp); 
}

static int read_frame(FILE *fp)
{
        struct v4l2_buffer buf;
        CLEAR(buf);

        buf.type = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf_type) {
            struct v4l2_plane planes[FMT_NUM_PLANES];
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) 
                errno_exit("VIDIOC_DQBUF");

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf_type) {
            printf("multi-planes bytesused %d\n", buf.m.planes[0].bytesused);
            process_image(buffers[buf.index].start, buf.m.planes[0].bytesused);
        } else {
            printf("bytesused %d\n", buf.m.planes[0].bytesused);
            process_image(buffers[buf.index].start, buf.bytesused);
        }

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF"); 

        return 1;
}

static void mainloop(void)
{
        unsigned int count;
        count = frame_count;
        float exptime, expgain, newexptime=0.0f;
        while (count-- > 0) {
            printf("No.%d\n",frame_count - count);        //显示当前帧数目
            // examples show how to use 3A interfaces
            newexptime += 0.003f;
            if (count > 10)
                rkisp_setManualGainAndTime((void*&)g_3A_control_params, 8.0, newexptime);
            else
                rkisp_setAeMode((void*&)g_3A_control_params, HAL_AE_OPERATION_MODE_AUTO);
            rkisp_getAeTime((void*&)g_3A_control_params, exptime);
            rkisp_getAeGain((void*&)g_3A_control_params, expgain);
            rkisp_getAeMaxExposureGain((void*&)g_3A_control_params, expgain);
            rkisp_getAeMaxExposureTime((void*&)g_3A_control_params, exptime);
            read_frame(fp);
        }
        printf("\nREAD AND SAVE DONE!\n");
}

static void stop_capturing(void)
{
        enum v4l2_buf_type type;

    	if (_RKIspFunc.stop_func != NULL) {
    	    printf ("stop rkisp engine\n");
    	    _RKIspFunc.stop_func(_rkisp_engine);
    	}
        type = buf_type;
        if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
            errno_exit("VIDIOC_STREAMOFF");
}

#define MAX_MEDIA_INDEX 64
static struct media_device* __rkisp_get_media_dev_by_vnode(const char* vnode) {
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
            if (!strcmp (devname, vnode)) {
                  goto out;
            }
          }
        }
        media_device_unref (device);
    }

out:
    return device;
}


static void start_capturing(void)
{
        unsigned int i;
        struct RKisp_media_ctl rkisp;
        enum v4l2_buf_type type;

    	if (_RKIspFunc.init_func != NULL) {
			_RKIspFunc.init_func(&_rkisp_engine, iq_file,
                               (cl_result_callback_ops_t*)(g_3A_control_params));
		}
    	if (_RKIspFunc.prepare_func != NULL) {
			struct rkisp_cl_prepare_params_s params={0};
            int nents;

            rkisp.controller =
                __rkisp_get_media_dev_by_vnode (dev_name);
            if (!rkisp.controller)
                errno_exit(
                    "Can't find controller, maybe use a wrong video-node or wrong permission to media node");
            rkisp.isp_subdev =
              media_get_entity_by_name (rkisp.controller, "rkisp1-isp-subdev",
                                        strlen("rkisp1-isp-subdev"));
            rkisp.isp_params_dev =
              media_get_entity_by_name (rkisp.controller, "rkisp1-input-params",
                                        strlen("rkisp1-input-params"));
            rkisp.isp_stats_dev =
              media_get_entity_by_name (rkisp.controller, "rkisp1-statistics",
                                        strlen("rkisp1-statistics"));
             /* assume the last enity is sensor_subdev */
            nents = media_get_entities_count (rkisp.controller);
            rkisp.sensor_subdev = media_get_entity (rkisp.controller, nents - 1);

            params.isp_sd_node_path = media_entity_get_devname (rkisp.isp_subdev);
            params.isp_vd_params_path = media_entity_get_devname (rkisp.isp_params_dev);
            params.isp_vd_stats_path = media_entity_get_devname (rkisp.isp_stats_dev);
            params.sensor_sd_node_path = media_entity_get_devname (rkisp.sensor_subdev);
            params.staticMeta =
                g_3A_control_params->_settings_metadata.getAndLock();
            /*
            // isp subdev node path
            params.isp_sd_node_path="/dev/v4l-subdev0";
            // isp params video node path
            params.isp_vd_params_path="/dev/video3";
            // isp statistics video node path
            params.isp_vd_stats_path="/dev/video2";
            // camera sensor subdev node path
            params.sensor_sd_node_path="/dev/v4l-subdev2";
            */
			_RKIspFunc.prepare_func(_rkisp_engine, &params);
            g_3A_control_params->_settings_metadata.unlock(params.staticMeta);
            media_device_unref (rkisp.controller);
		}

        // set initial user params
        if (_RKIspFunc.set_frame_params_func != NULL) {
            _RKIspFunc.set_frame_params_func(_rkisp_engine,
                                             &g_3A_control_params->_frame_metas);
        }

    	if (_RKIspFunc.start_func != NULL) {
    	    printf ("device manager start, capture dev fd: %d\n", fd);
    	    _RKIspFunc.start_func(_rkisp_engine);
    	    printf ("device manager isp_init\n");

    	    if (_rkisp_engine == NULL) {
    	        printf ("rkisp_init engine failed\n");
    	    } else {
    	        printf ("rkisp_init engine succeed\n");
    	    }
    	}
        for (i = 0; i < n_buffers; ++i) {
                struct v4l2_buffer buf;

                CLEAR(buf);
                buf.type = buf_type;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;

                if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf_type) {
                    struct v4l2_plane planes[FMT_NUM_PLANES];
                    buf.m.planes = planes;
                    buf.length = FMT_NUM_PLANES;
                }
                if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                        errno_exit("VIDIOC_QBUF");
        }
        type = buf_type;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
        unsigned int i;

        for (i = 0; i < n_buffers; ++i)
                if (-1 == munmap(buffers[i].start, buffers[i].length))
                        errno_exit("munmap");

        free(buffers);

        if (_RKIspFunc.deinit_func != NULL) {
            _RKIspFunc.deinit_func(_rkisp_engine);
            deinit_3A_control_params();
        }

        dlclose(_RKIspFunc.rkisp_handle);
}



static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 4;
        req.type = buf_type;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                                 "memory mapping\n", dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) {
                fprintf(stderr, "Insufficient buffer memory on %s\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        buffers = (struct buffer*)calloc(req.count, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;
                struct v4l2_plane planes[FMT_NUM_PLANES];
                CLEAR(buf);
                CLEAR(planes);

                buf.type = buf_type;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = n_buffers;

                if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf_type) {
                    buf.m.planes = planes;
                    buf.length = FMT_NUM_PLANES;
                }

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf_type) {
                    buffers[n_buffers].length = buf.m.planes[0].length;
                    buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.m.planes[0].length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.planes[0].m.mem_offset);
                } else {
                    buffers[n_buffers].length = buf.length;
                    buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);
                }

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
        }
}


static void init_device(void)
{
        struct v4l2_capability cap;
        struct v4l2_format fmt;

        if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s is no V4L2 device\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_QUERYCAP");
                }
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
                !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
            fprintf(stderr, "%s is not a video capture device, capabilities: %x\n",
                         dev_name, cap.capabilities);
                exit(EXIT_FAILURE);
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                fprintf(stderr, "%s does not support streaming i/o\n",
                    dev_name);
                exit(EXIT_FAILURE);
        }



        CLEAR(fmt);
        if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
            buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        fmt.type = buf_type;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = format;
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
                errno_exit("VIDIOC_S_FMT");

        init_mmap();

		//INIT RKISP
        _RKIspFunc.rkisp_handle = dlopen(LIBRKISP, RTLD_NOW);
    	if (_RKIspFunc.rkisp_handle == NULL) {
            printf ("open %s failed\n", LIBRKISP);
    	} else {
            printf ("open %s successed\n", LIBRKISP);
    	    _RKIspFunc.init_func=(rkisp_init_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_init");
    	    _RKIspFunc.prepare_func=(rkisp_prepare_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_prepare");
    	    _RKIspFunc.start_func=(rkisp_start_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_start");
    	    _RKIspFunc.stop_func=(rkisp_stop_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_stop");
    	    _RKIspFunc.deinit_func=(rkisp_deinit_func)dlsym(_RKIspFunc.rkisp_handle, "rkisp_cl_deinit");
            _RKIspFunc.set_frame_params_func=(rkisp_cl_set_frame_params_func)dlsym(_RKIspFunc.rkisp_handle,
                                                                      "rkisp_cl_set_frame_params");
    	    if (_RKIspFunc.start_func == NULL) {
    	        printf ("func rkisp_start not found.");
    	        const char *errmsg;
    	        if ((errmsg = dlerror()) != NULL) {
    	            printf("dlsym rkisp_start fail errmsg: %s", errmsg);
    	        }
    	    } else {
                printf("dlsym rkisp_start success\n");
    	    }
            init_3A_control_params();
    	}

}

static void close_device(void)
{
        if (-1 == close(fd))
                errno_exit("close");

        fd = -1;
}

static void open_device(void)
{
        fd = open(dev_name, O_RDWR /* required */ /*| O_NONBLOCK*/, 0);

        if (-1 == fd) {
                fprintf(stderr, "Cannot open '%s': %d, %s\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }
}

void parse_args(int argc, char **argv)
{
   int c;
   int digit_optind = 0;

   while (1) {
       int this_option_optind = optind ? optind : 1;
       int option_index = 0;
       static struct option long_options[] = {
           {"width",    required_argument, 0, 'w' },
           {"height",   required_argument, 0, 'h' },
           {"format",   required_argument, 0, 'f' },
           {"iqfile",   required_argument, 0, 'i' },
           {"device",   required_argument, 0, 'd' },
           {"output",   required_argument, 0, 'o' },
           {"count",    required_argument, 0, 'c' },
           {"help",     no_argument,       0, 'p' },
           {"verbose",  no_argument,       0, 'v' },
           {0,          0,                 0,  0  }
       };

       c = getopt_long(argc, argv, "w:h:f:i:d:o:pv",
           long_options, &option_index);
       if (c == -1)
           break;

       switch (c) {
       case 'c':
           frame_count = atoi(optarg);
           break;
       case 'w':
           width = atoi(optarg);
           break;
       case 'h':
           height = atoi(optarg);
           break;
       case 'f':
           format = v4l2_fourcc(optarg[0], optarg[1], optarg[2], optarg[3]);
           break;
       case 'i':
           strcpy(iq_file, optarg);
           break;
       case 'd':
           strcpy(dev_name, optarg);
           break;
       case 'o':
           strcpy(out_file, optarg);
           break;
       case '?':
       case 'p':
           printf("Usage: %s to capture rkisp1 frames\n"
                  "         --width,  default 640,             optional, width of image\n"
                  "         --height, default 480,             optional, height of image\n"
                  "         --format, default NV12,            optional, fourcc of format\n"
                  "         --count,  default    5,            optional, how many frames to capture\n"
                  "         --iqfile, default /etc/cam_iq.xml, optional, camera IQ file\n"
                  "         --device,                          required, path of video device\n"
                  "         --output,                          required, output file path\n"
                  "         --verbose,                         optional, print more log\n",
                  argv[0]);
           exit(-1);

       default:
           printf("?? getopt returned character code 0%o ??\n", c);
       }
   }

   if (strlen(out_file) == 0 || strlen(dev_name) == 0) {
        fprintf(stderr, "arguments --output and --device are required\n");
        exit(-1);
   }

}

int main(int argc, char **argv)
{
        parse_args(argc, argv);

        if ((fp = fopen(out_file, "w")) == NULL) {
            perror("Creat file failed");
            exit(0);
        }
        open_device();
        init_device();
        start_capturing();
        mainloop();
        fclose(fp);
        stop_capturing();
        uninit_device();
        close_device();
        return 0;
}
