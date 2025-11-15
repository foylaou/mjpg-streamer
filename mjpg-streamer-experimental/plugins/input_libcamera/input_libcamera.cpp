/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2025                                                      #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>

#include <linux/videodev2.h>

#include <libcamera/libcamera.h>
#include <memory>
#include <iostream>
#include <chrono>
#include <queue>
#include <mutex>

extern "C" {
#include "../../mjpg_streamer.h"
#include "../../utils.h"
}

#define INPUT_PLUGIN_NAME "libcamera input plugin"

using namespace libcamera;
using namespace std::chrono_literals;

/* private functions and variables to this plugin */
static pthread_t worker;
static globals *pglobal;
static int plugin_number;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

/* default parameters */
static int width = 640;
static int height = 480;
static int fps = 30;
static int quality = 85;
static int camera_id = 0;

/* libcamera objects */
class CameraContext {
public:
    std::unique_ptr<CameraManager> cm;
    std::shared_ptr<Camera> camera;
    std::unique_ptr<FrameBufferAllocator> allocator;
    std::unique_ptr<CameraConfiguration> config;
    std::vector<std::unique_ptr<Request>> requests;
    std::queue<Request *> completed_requests;
    std::mutex queue_mutex;
    bool running;

    CameraContext() : running(false) {}
};

static CameraContext *ctx = nullptr;

/******************************************************************************
Description.: print help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void) {
    fprintf(stderr,
    " ---------------------------------------------------------------\n" \
    " Help for input plugin..: " INPUT_PLUGIN_NAME "\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-fps | --framerate]...: set video framerate, default: %d\n" \
    " [-x | --width].........: width of frame capture, default: %d\n" \
    " [-y | --height]........: height of frame capture, default: %d\n" \
    " [-quality].............: set JPEG quality 0-100, default: %d\n" \
    " [-camera]...............: camera device number, default: %d\n" \
    " ---------------------------------------------------------------\n",
    fps, width, height, quality, camera_id);
}

/******************************************************************************
Description.: Request completion callback
Input Value.: request - completed request
Return Value: -
******************************************************************************/
static void requestComplete(Request *request) {
    if (!ctx) return;

    if (request->status() == Request::RequestCancelled)
        return;

    std::lock_guard<std::mutex> lock(ctx->queue_mutex);
    ctx->completed_requests.push(request);
}

/******************************************************************************
Description.: Initialize libcamera
Input Value.: -
Return Value: 0 on success, -1 on error
******************************************************************************/
static int init_camera() {
    ctx = new CameraContext();

    /* Create camera manager */
    ctx->cm = std::make_unique<CameraManager>();
    int ret = ctx->cm->start();
    if (ret) {
        IPRINT("Failed to start camera manager: %d\n", ret);
        return -1;
    }

    /* Get camera list */
    auto cameras = ctx->cm->cameras();
    if (cameras.empty()) {
        IPRINT("No cameras available\n");
        return -1;
    }

    /* Select camera */
    if (camera_id >= (int)cameras.size()) {
        IPRINT("Camera %d not available (only %zu cameras found)\n", camera_id, cameras.size());
        return -1;
    }

    std::string camera_name = cameras[camera_id]->id();
    ctx->camera = ctx->cm->get(camera_name);
    if (!ctx->camera) {
        IPRINT("Failed to get camera %s\n", camera_name.c_str());
        return -1;
    }

    /* Acquire camera */
    if (ctx->camera->acquire()) {
        IPRINT("Failed to acquire camera\n");
        return -1;
    }

    IPRINT("Using camera: %s\n", camera_name.c_str());

    /* Configure camera */
    ctx->config = ctx->camera->generateConfiguration({StreamRole::VideoRecording});
    if (!ctx->config) {
        IPRINT("Failed to generate camera configuration\n");
        return -1;
    }

    StreamConfiguration &streamConfig = ctx->config->at(0);
    streamConfig.size.width = width;
    streamConfig.size.height = height;
    streamConfig.pixelFormat = formats::MJPEG;

    /* Validate configuration */
    CameraConfiguration::Status validation = ctx->config->validate();
    if (validation == CameraConfiguration::Invalid) {
        IPRINT("Camera configuration invalid\n");
        return -1;
    } else if (validation == CameraConfiguration::Adjusted) {
        IPRINT("Camera configuration adjusted to %dx%d\n",
               streamConfig.size.width, streamConfig.size.height);
        width = streamConfig.size.width;
        height = streamConfig.size.height;
    }

    /* Apply configuration */
    if (ctx->camera->configure(ctx->config.get())) {
        IPRINT("Failed to configure camera\n");
        return -1;
    }

    /* Allocate buffers */
    ctx->allocator = std::make_unique<FrameBufferAllocator>(ctx->camera);
    Stream *stream = streamConfig.stream();

    if (ctx->allocator->allocate(stream) < 0) {
        IPRINT("Failed to allocate buffers\n");
        return -1;
    }

    /* Create requests */
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = ctx->allocator->buffers(stream);
    for (unsigned int i = 0; i < buffers.size(); ++i) {
        std::unique_ptr<Request> request = ctx->camera->createRequest();
        if (!request) {
            IPRINT("Failed to create request\n");
            return -1;
        }

        const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
        if (request->addBuffer(stream, buffer.get())) {
            IPRINT("Failed to add buffer to request\n");
            return -1;
        }

        /* Set frame rate control */
        ControlList &controls = request->controls();
        int64_t frame_time = 1000000 / fps; // microseconds
        controls.set(controls::FrameDurationLimits, {frame_time, frame_time});

        ctx->requests.push_back(std::move(request));
    }

    /* Connect signal */
    ctx->camera->requestCompleted.connect(requestComplete);

    IPRINT("Camera initialized successfully: %dx%d @ %d fps\n", width, height, fps);

    return 0;
}

/******************************************************************************
Description.: Start camera capture
Input Value.: -
Return Value: 0 on success, -1 on error
******************************************************************************/
static int start_camera() {
    if (!ctx || !ctx->camera) {
        IPRINT("Camera not initialized\n");
        return -1;
    }

    /* Start camera */
    if (ctx->camera->start()) {
        IPRINT("Failed to start camera\n");
        return -1;
    }

    /* Queue all requests */
    for (std::unique_ptr<Request> &request : ctx->requests) {
        if (ctx->camera->queueRequest(request.get())) {
            IPRINT("Failed to queue request\n");
            return -1;
        }
    }

    ctx->running = true;
    IPRINT("Camera started\n");

    return 0;
}

/******************************************************************************
Description.: Stop camera capture
Input Value.: -
Return Value: -
******************************************************************************/
static void stop_camera() {
    if (!ctx || !ctx->camera) return;

    if (ctx->running) {
        ctx->camera->stop();
        ctx->running = false;
        IPRINT("Camera stopped\n");
    }

    ctx->camera->requestCompleted.disconnect(requestComplete);
    ctx->allocator.reset();
    ctx->requests.clear();

    ctx->camera->release();
    ctx->camera.reset();

    ctx->cm->stop();
    ctx->cm.reset();

    delete ctx;
    ctx = nullptr;
}

/******************************************************************************
Description.: Copy a picture from the buffer and signal fresh_frame
Input Value.: -
Return Value: -
******************************************************************************/
static void copy_frame(const uint8_t *buffer, size_t length) {
    pthread_mutex_lock(&pglobal->in[plugin_number].db);

    /* Free old buffer */
    if (pglobal->in[plugin_number].buf != NULL) {
        free(pglobal->in[plugin_number].buf);
    }

    /* Allocate new buffer */
    pglobal->in[plugin_number].buf = (unsigned char *)malloc(length);
    if (pglobal->in[plugin_number].buf == NULL) {
        IPRINT("Failed to allocate memory for frame\n");
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);
        return;
    }

    /* Copy frame data */
    memcpy(pglobal->in[plugin_number].buf, buffer, length);
    pglobal->in[plugin_number].size = length;

    /* Update timestamp */
    gettimeofday(&pglobal->in[plugin_number].timestamp, NULL);

    /* Signal fresh frame */
    pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
    pthread_mutex_unlock(&pglobal->in[plugin_number].db);
}

/******************************************************************************
Description.: Read JPEG data from frame buffer
Input Value.: fb - frame buffer
Return Value: -
******************************************************************************/
static void process_frame(FrameBuffer *fb) {
    const FrameBuffer::Plane &plane = fb->planes()[0];
    int fd = plane.fd.get();
    size_t length = plane.length;

    /* Map the buffer */
    void *mem = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        IPRINT("Failed to mmap buffer\n");
        return;
    }

    /* Copy JPEG data */
    const FrameMetadata &metadata = fb->metadata();
    size_t bytes_used = metadata.planes()[0].bytesused;

    copy_frame((const uint8_t *)mem, bytes_used);

    munmap(mem, length);
}

/******************************************************************************
Description.: Worker thread, captures frames and stores them in global buffer
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg) {
    /* Set thread name */
    pthread_setname_np(pthread_self(), "libcamera");

    /* Initialize camera */
    if (init_camera() < 0) {
        IPRINT("Failed to initialize camera\n");
        return NULL;
    }

    /* Start capture */
    if (start_camera() < 0) {
        stop_camera();
        return NULL;
    }

    /* Main loop */
    while (!pglobal->stop) {
        Request *request = nullptr;

        /* Get completed request */
        {
            std::lock_guard<std::mutex> lock(ctx->queue_mutex);
            if (!ctx->completed_requests.empty()) {
                request = ctx->completed_requests.front();
                ctx->completed_requests.pop();
            }
        }

        if (request) {
            /* Process frame */
            FrameBuffer *buffer = request->buffers().begin()->second;
            process_frame(buffer);

            /* Requeue request */
            request->reuse(Request::ReuseBuffers);
            if (ctx->camera->queueRequest(request)) {
                IPRINT("Failed to requeue request\n");
                break;
            }
        } else {
            /* No frames available, sleep briefly */
            usleep(1000);
        }
    }

    /* Cleanup */
    stop_camera();

    return NULL;
}

/******************************************************************************
Description.: Cleanup thread
Input Value.: arg is not used
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg) {
    static unsigned char first_run = 1;

    if (!first_run) {
        DBG("Already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("Cleaning up resources allocated by worker thread\n");

    if (pglobal->in[plugin_number].buf != NULL) {
        free(pglobal->in[plugin_number].buf);
        pglobal->in[plugin_number].buf = NULL;
    }
}

/******************************************************************************
Description.: This function is called from the main program when the plugin
              is loaded for the first time
Input Value.: All the parameters work like argc and argv
Return Value: 0 if everything is OK, other values signal an error
******************************************************************************/
extern "C" int input_init(input_parameter *param, int id) {
    int i;
    plugin_number = id;

    /* Initialize global variables */
    pglobal = param->global;

    /* Display the parsed parameters */
    IPRINT("---------------------------------------------------------------\n");
    IPRINT("Input plugin.....: " INPUT_PLUGIN_NAME "\n");

    /* Parse parameters */
    for (i = 0; i < param->argc; i++) {
        char *arg = param->argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            help();
            return 1;
        } else if (strcmp(arg, "-fps") == 0 || strcmp(arg, "--framerate") == 0) {
            if (i + 1 >= param->argc) {
                IPRINT("No value specified for %s\n", arg);
                return 1;
            }
            fps = atoi(param->argv[++i]);
        } else if (strcmp(arg, "-x") == 0 || strcmp(arg, "--width") == 0) {
            if (i + 1 >= param->argc) {
                IPRINT("No value specified for %s\n", arg);
                return 1;
            }
            width = atoi(param->argv[++i]);
        } else if (strcmp(arg, "-y") == 0 || strcmp(arg, "--height") == 0) {
            if (i + 1 >= param->argc) {
                IPRINT("No value specified for %s\n", arg);
                return 1;
            }
            height = atoi(param->argv[++i]);
        } else if (strcmp(arg, "-quality") == 0) {
            if (i + 1 >= param->argc) {
                IPRINT("No value specified for quality\n");
                return 1;
            }
            quality = atoi(param->argv[++i]);
        } else if (strcmp(arg, "-camera") == 0) {
            if (i + 1 >= param->argc) {
                IPRINT("No value specified for camera\n");
                return 1;
            }
            camera_id = atoi(param->argv[++i]);
        }
    }

    IPRINT("Desired Resolution: %d x %d @ %d fps\n", width, height, fps);
    IPRINT("JPEG Quality......: %d\n", quality);
    IPRINT("Camera ID.........: %d\n", camera_id);
    IPRINT("---------------------------------------------------------------\n");

    return 0;
}

/******************************************************************************
Description.: Stops the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
extern "C" int input_stop(int id) {
    DBG("Will stop worker thread\n");

    if (pglobal->stop) {
        return 0;
    }

    /* Signal worker thread to stop */
    pglobal->stop = 1;

    /* Wait for worker thread */
    pthread_join(worker, NULL);

    return 0;
}

/******************************************************************************
Description.: Starts the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
extern "C" int input_run(int id) {
    pglobal->in[id].buf = NULL;
    pglobal->in[id].size = 0;

    if (pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        worker_cleanup(NULL);
        IPRINT("Could not start worker thread\n");
        return -1;
    }
    pthread_detach(worker);

    return 0;
}

/******************************************************************************
Description.: Process commands
Input Value.: -
Return Value: 0
******************************************************************************/
extern "C" int input_cmd(int plugin, unsigned int control_id, unsigned int group, int value, char *value_str) {
    DBG("Received command: plugin %d, control %d, group %d, value %d\n",
        plugin, control_id, group, value);

    /* Commands not yet implemented */
    return 0;
}
