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

#include <jpeglib.h>

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
    IPRINT("Initializing libcamera context...\n");
    ctx = new CameraContext();

    IPRINT("Creating camera manager...\n");
    /* Create camera manager */
    ctx->cm = std::make_unique<CameraManager>();

    IPRINT("Starting camera manager...\n");
    int ret = ctx->cm->start();
    if (ret) {
        IPRINT("Failed to start camera manager: %d\n", ret);
        return -1;
    }

    IPRINT("Getting camera list...\n");
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

    /* Use RGB888 format */
    streamConfig.pixelFormat = formats::RGB888;

    /* Validate configuration */
    CameraConfiguration::Status validation = ctx->config->validate();
    if (validation == CameraConfiguration::Invalid) {
        IPRINT("Camera configuration invalid\n");
        return -1;
    } else if (validation == CameraConfiguration::Adjusted) {
        IPRINT("Camera configuration adjusted to %dx%d, format: %s\n",
               streamConfig.size.width, streamConfig.size.height,
               streamConfig.pixelFormat.toString().c_str());
        width = streamConfig.size.width;
        height = streamConfig.size.height;
    }

    /* Check format */
    IPRINT("Requested pixel format: %s\n", streamConfig.pixelFormat.toString().c_str());

    /* Apply configuration */
    if (ctx->camera->configure(ctx->config.get())) {
        IPRINT("Failed to configure camera\n");
        return -1;
    }

    /* Check what we actually got after configuration */
    IPRINT("Final pixel format: %s (%dx%d, stride: %u)\n",
           streamConfig.pixelFormat.toString().c_str(),
           streamConfig.size.width, streamConfig.size.height,
           streamConfig.stride);
    IPRINT("Will encode to JPEG in software\n");

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
Description.: Encode RGB to JPEG with R/B swap
Input Value.: rgb_data - pointer to RGB888 data
              width, height - image dimensions
              jpeg_data - output buffer pointer (will be allocated)
              jpeg_size - output size pointer
Return Value: 0 on success, -1 on error
******************************************************************************/
static int rgb_to_jpeg(const uint8_t *rgb_data, int img_width, int img_height,
                       uint8_t **jpeg_data, size_t *jpeg_size) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned long outlen = 0;
    uint8_t *outbuffer = NULL;

    if (!rgb_data) {
        IPRINT("RGB data is NULL\n");
        return -1;
    }

    /* Allocate line buffer for R/B swap */
    uint8_t *line_buffer = (uint8_t *)malloc(img_width * 3);
    if (!line_buffer) {
        IPRINT("Failed to allocate line buffer\n");
        return -1;
    }

    /* Initialize JPEG compression */
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    /* Set output to memory */
    jpeg_mem_dest(&cinfo, &outbuffer, &outlen);

    /* Set parameters for RGB output */
    cinfo.image_width = img_width;
    cinfo.image_height = img_height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    /* Start compression */
    jpeg_start_compress(&cinfo, TRUE);

    /* Compress each scanline with R/B swap */
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *src = rgb_data + (cinfo.next_scanline * img_width * 3);
        uint8_t *dst = line_buffer;

        /* Swap R and B channels */
        for (int x = 0; x < img_width; x++) {
            dst[0] = src[2];  // R ← B
            dst[1] = src[1];  // G ← G
            dst[2] = src[0];  // B ← R
            src += 3;
            dst += 3;
        }

        row_pointer[0] = line_buffer;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    /* Finish compression */
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    free(line_buffer);

    /* Return result */
    *jpeg_data = outbuffer;
    *jpeg_size = outlen;

    return 0;
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
Description.: Read RGB data from frame buffer and encode to JPEG
Input Value.: fb - frame buffer
Return Value: -
******************************************************************************/
static void process_frame(FrameBuffer *fb) {
    static int frame_num = 0;
    frame_num++;

    /* Get number of planes */
    unsigned int num_planes = fb->planes().size();

    if (frame_num == 1) {
        IPRINT("Frame has %u planes\n", num_planes);
    }

    /* Handle single-plane RGB888 format */
    if (num_planes != 1) {
        IPRINT("Unexpected number of planes: %u (expected 1 for RGB888)\n", num_planes);
        return;
    }

    /* Map RGB plane */
    const FrameBuffer::Plane &rgb_plane = fb->planes()[0];
    size_t rgb_size = rgb_plane.length;
    void *rgb_mem = mmap(NULL, rgb_size, PROT_READ, MAP_SHARED, rgb_plane.fd.get(), 0);
    if (rgb_mem == MAP_FAILED) {
        IPRINT("Failed to mmap RGB plane\n");
        return;
    }

    if (frame_num == 1) {
        IPRINT("Mapped RGB plane: %zu bytes (expected: %zu)\n", rgb_size, (size_t)width * height * 3);

        /* Debug: Print first 10 pixels */
        const uint8_t *debug_data = (const uint8_t *)rgb_mem;
        IPRINT("First 10 pixels (raw data):\n");
        for (int i = 0; i < 10; i++) {
            IPRINT("  Pixel %d: [%3d, %3d, %3d]\n", i,
                   debug_data[i*3], debug_data[i*3+1], debug_data[i*3+2]);
        }
    }

    /* Encode RGB to JPEG (with R/B swap) */
    uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0;

    if (rgb_to_jpeg((const uint8_t *)rgb_mem, width, height, &jpeg_data, &jpeg_size) == 0) {
        /* Copy JPEG data to global buffer */
        copy_frame(jpeg_data, jpeg_size);

        /* Free JPEG buffer (allocated by jpeg_mem_dest) */
        free(jpeg_data);

        if (frame_num == 1) {
            IPRINT("First frame encoded successfully: %zu bytes\n", jpeg_size);
        }
    } else {
        IPRINT("Frame %d: Failed to encode JPEG\n", frame_num);
    }

    /* Unmap RGB plane */
    munmap(rgb_mem, rgb_size);
}

/******************************************************************************
Description.: Worker thread, captures frames and stores them in global buffer
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg) {
    IPRINT("Worker thread started\n");

    /* Set thread name */
    pthread_setname_np(pthread_self(), "libcamera");

    IPRINT("Calling init_camera()...\n");
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
    IPRINT("Entering main capture loop...\n");
    int frame_count = 0;
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
            frame_count++;
            if (frame_count % 30 == 1) {  /* Print every 30 frames */
                IPRINT("Processing frame #%d\n", frame_count);
            }

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

    IPRINT("Exiting main loop, processed %d frames\n", frame_count);

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
    IPRINT("Parsing %d parameters...\n", param->argc);
    for (i = 0; i < param->argc; i++) {
        char *arg = param->argv[i];
        IPRINT("  Parameter %d: %s\n", i, arg ? arg : "NULL");

        /* Skip NULL parameters */
        if (arg == NULL) {
            continue;
        }

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
    IPRINT("input_run() called with id=%d\n", id);

    pglobal->in[id].buf = NULL;
    pglobal->in[id].size = 0;

    IPRINT("Creating worker thread...\n");
    if (pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        worker_cleanup(NULL);
        IPRINT("Could not start worker thread\n");
        return -1;
    }
    pthread_detach(worker);

    IPRINT("Worker thread created successfully\n");
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
