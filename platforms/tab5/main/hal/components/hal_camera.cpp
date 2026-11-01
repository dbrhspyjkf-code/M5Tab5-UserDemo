/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include "../utils/task_controller/task_controller.h"
#include <mooncake_log.h>
#include <vector>
#include <driver/gpio.h>
#include <memory>
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "imlib.h"
#include "freertos/queue.h"
#include "jpg/image_to_jpeg.h"   // RGB565→JPEG (xiaozhi_core, esp_new_jpeg)
#include "esp_video_isp_ioctl.h"   // esp_video_isp_wb_t (AWB)

#define CAMERA_WIDTH  1280
#define CAMERA_HEIGHT 720

static lv_obj_t* camera_canvas;
static QueueHandle_t queue_camera_ctrl = NULL;
// 定义任务控制标志
#define TASK_CONTROL_PAUSE  0
#define TASK_CONTROL_RESUME 1
#define TASK_CONTROL_EXIT   2

static bool is_camera_capturing = false;
static std::mutex camera_mutex;

// 2026-11-01 防御化重写: 原 ESP_ERROR_CHECK(esp_video_init/new_cam/ppa) 任一
// 失败即 abort 重启 (拍照识别点击重启根因候选); img_show_data 分配失败继续
// PPA 写 NULL = StoreProhibited; stopCameraCapture 异步不等 task 退出, 调用方
// 删 canvas 后 task 仍在 lv_canvas_set_buffer = UAF。现全部改干净失败路径 +
// stop 同步等 task 退出 (信号量, 2s 超时) + DQBUF 非阻塞 (断流可响应 stop)。
static TaskHandle_t camera_task      = NULL;
static SemaphoreHandle_t cam_exit_done = NULL;   // task 真退出时 give

// ── 快门拍照 (capturePhotoJpeg) ────────────────────────────────────────────
// 预览 task 每帧 PPA 输出后检查请求标志, 命中则把已 mirror 的 RGB565 帧
// 拷到共享缓冲并给信号量; 调用线程等信号量后编码 JPEG —— 帧数据无跨线程
// 竞争 (编码期间预览 task 已拿到新帧继续跑)。
static uint8_t* s_photo_frame   = nullptr;   // 1280*720*2, 首拍时分配
static SemaphoreHandle_t s_photo_sem = NULL;
static volatile bool s_photo_req = false;

static const char* TAG = "camera";

#define EXAMPLE_VIDEO_BUFFER_COUNT 2
#define MEMORY_TYPE                V4L2_MEMORY_MMAP
#define CAM_DEV_PATH               ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

typedef struct cam {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t* buffer[EXAMPLE_VIDEO_BUFFER_COUNT];
} cam_t;

typedef enum {
    EXAMPLE_VIDEO_FMT_RAW8   = V4L2_PIX_FMT_SBGGR8,
    EXAMPLE_VIDEO_FMT_RAW10  = V4L2_PIX_FMT_SBGGR10,
    EXAMPLE_VIDEO_FMT_GREY   = V4L2_PIX_FMT_GREY,
    EXAMPLE_VIDEO_FMT_RGB565 = V4L2_PIX_FMT_RGB565,
    EXAMPLE_VIDEO_FMT_RGB888 = V4L2_PIX_FMT_RGB24,
    EXAMPLE_VIDEO_FMT_YUV422 = V4L2_PIX_FMT_YUV422P,
    EXAMPLE_VIDEO_FMT_YUV420 = V4L2_PIX_FMT_YUV420,
} example_fmt_t;

// O_NONBLOCK: DQBUF 无帧时立即 EAGAIN, 循环可继续响应 stop 消息 (原阻塞式
// 在断流时 stop 永远收不到 → task 悬死 + canvas UAF)
int app_video_open(char* dev, example_fmt_t init_fmt)
{
    struct v4l2_format default_format;
    struct v4l2_capability capability;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open video failed");
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        ESP_LOGE(TAG, "failed to get capability");
        goto exit_0;
    }

    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability.version >> 16), (uint8_t)(capability.version >> 8),
             (uint8_t)capability.version);
    ESP_LOGI(TAG, "driver:  %s", capability.driver);
    ESP_LOGI(TAG, "card:    %s", capability.card);
    ESP_LOGI(TAG, "bus:     %s", capability.bus_info);

    memset(&default_format, 0, sizeof(struct v4l2_format));
    default_format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &default_format) != 0) {
        ESP_LOGE(TAG, "failed to get format");
        goto exit_0;
    }

    ESP_LOGI(TAG, "width=%" PRIu32 " height=%" PRIu32, default_format.fmt.pix.width, default_format.fmt.pix.height);

    if (default_format.fmt.pix.pixelformat != init_fmt) {
        struct v4l2_format format = {.type = type,
                                     .fmt  = {.pix = {.width       = default_format.fmt.pix.width,
                                                      .height      = default_format.fmt.pix.height,
                                                      .pixelformat = init_fmt}}};

        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG, "failed to set format");
            goto exit_0;
        }
    }

    return fd;
exit_0:
    close(fd);
    return -1;
}

static esp_err_t new_cam(int cam_fd, cam_t** ret_wc)
{
    int ret;
    struct v4l2_format format;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_requestbuffers req;
    cam_t* wc;

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed get fmt");
        return ESP_FAIL;
    }

    wc = (cam_t*)calloc(1, sizeof(cam_t));
    if (!wc) {
        return ESP_ERR_NO_MEM;
    }

    wc->fd           = cam_fd;
    wc->width        = format.fmt.pix.width;
    wc->height       = format.fmt.pix.height;
    wc->pixel_format = format.fmt.pix.pixelformat;

    memset(&req, 0, sizeof(req));
    req.count  = ARRAY_SIZE(wc->buffer);
    req.type   = type;
    req.memory = MEMORY_TYPE;
    if (ioctl(wc->fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "failed to req buffers");
        ret = ESP_FAIL;
        goto errout;
    }

    for (int i = 0; i < ARRAY_SIZE(wc->buffer); i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type   = type;
        buf.memory = MEMORY_TYPE;
        buf.index  = i;
        if (ioctl(wc->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to query buffer");
            ret = ESP_FAIL;
            goto errout;
        }

        wc->buffer[i] = (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, wc->fd, buf.m.offset);
        if (!wc->buffer[i]) {
            ESP_LOGE(TAG, "failed to map buffer");
            ret = ESP_FAIL;
            goto errout;
        }

        if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to queue frame buffer");
            ret = ESP_FAIL;
            goto errout;
        }
    }

    if (ioctl(wc->fd, VIDIOC_STREAMON, &type)) {
        ESP_LOGE(TAG, "failed to start stream");
        ret = ESP_FAIL;
        goto errout;
    }

    *ret_wc = wc;
    return ESP_OK;

errout:
    free(wc);
    return ret;
}

static bool cam_is_initial = false;
static cam_t* camera       = NULL;

// ── 软件 3A (2026-11-01, 参考社区三项目实测定论) ──────────────────────────
// SC202CS 上电最小曝光≈近黑帧, esp_ipa 自动 3A 未集成 (zacs/person_detector
// BRINGUP 实锤同款问题)。方案: 预览帧 Rec.601 luma 采样 → gain 索引步进闭环
// (目标 mean 110-130, person_detector 硬件验证 exposure=811→mean≈130); R/G/B
// 均值 → ISP WB 控件均衡 (治 G 通道 ~1.7× 的偏绿)。设置需周期重申 (会漂,
// person_detector v0.1.11 教训: STREAMON 时设的不持久)。曝光行数不动
// (拉长会糊), 只动 gain; 范围从 QUERY_EXT_CTRL 拿。
static int  s_gain_idx      = 0;
static int  s_gain_min      = 0;
static int  s_gain_max      = 0;
static bool s_gain_ok       = false;
static int  s_isp_fd        = -1;
static bool s_wb_warned     = false;

static void cam_3a_init(int fd)
{
    struct v4l2_query_ext_ctrl qctrl = {.id = V4L2_CID_GAIN};
    if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0) {
        s_gain_min = qctrl.minimum;
        s_gain_max = qctrl.maximum;
        s_gain_idx = qctrl.default_value;
        s_gain_ok  = true;
        ESP_LOGI(TAG, "gain ctrl: idx %d..%d def %d", s_gain_min, s_gain_max, s_gain_idx);
    } else {
        ESP_LOGW(TAG, "query gain ctrl failed (AE off)");
    }
    // ISP 节点 (WB 控件在 ISP 设备, 不需开流)。一次性固定 WB —— 实测偏色
    // R53/G89/B52 → rg≈1.68, bg≈1.71; 闭环版因 WB 增益语义未验证而振荡
    // (画面红绿白循环), 改固定值一次设定 (服务端灰世界校正兜底识别)。
    s_isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (s_isp_fd >= 0) {
        esp_video_isp_wb_t wb = {.enable = true, .red_gain = 1.65f, .blue_gain = 1.7f};
        struct v4l2_ext_control ec = {.id = V4L2_CID_USER_ESP_ISP_WB, .size = sizeof(wb)};
        ec.p_u8 = (uint8_t*)&wb;
        struct v4l2_ext_controls ecs = {.ctrl_class = V4L2_CTRL_CLASS_USER,
                                        .count = 1, .controls = &ec};
        if (ioctl(s_isp_fd, VIDIOC_S_EXT_CTRLS, &ecs) != 0) {
            ESP_LOGW(TAG, "set ISP WB failed (AWB off)");
        } else {
            ESP_LOGI(TAG, "fixed WB set: rg=1.65 bg=1.70");
        }
    } else {
        ESP_LOGW(TAG, "open ISP dev failed (AWB off)");
    }
}

static void cam_3a_set_gain(int fd)
{
    if (!s_gain_ok) return;
    struct v4l2_ext_control ec = {.id = V4L2_CID_GAIN, .value = s_gain_idx};
    struct v4l2_ext_controls ecs = {.ctrl_class = V4L2_CID_CAMERA_CLASS,
                                    .count = 1, .controls = &ec};
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs) != 0) {
        ESP_LOGW(TAG, "set gain idx=%d failed", s_gain_idx);
    }
}

// 预览帧统计 + 闭环步进 (每 ~0.5s 调用; data=RGB565 mirror 后帧)
static void cam_3a_step(int fd, const uint8_t* frame, size_t frame_bytes)
{
    const uint16_t* px = (const uint16_t*)frame;
    const int stride = 128;   // 1/128 下采样
    uint32_t n = 0, r = 0, g = 0, b = 0;
    const int cnt = (int)(frame_bytes / 2 / stride);
    for (int i = 0; i < cnt; i++) {
        uint16_t v = px[i * stride];
        r += (v >> 11) & 0x1F;
        g += (v >> 5) & 0x3F;
        b += v & 0x1F;
        n++;
    }
    if (!n) return;
    r /= n; g /= n; b /= n;
    // Rec.601 luma (565 分量归一到 8bit)
    const int luma = (r * 8 * 77 + g * 4 * 151 + b * 8 * 28) / 256;

    // AE: 死区 90-150 (宽死区防振荡), 步进 2 索引 (~6%/步), 到边界即稳。
    // 2026-11-01 首版 死区95-160+步进4 实测在带边来回过冲 (画面忽白忽暗)。
    if (s_gain_ok) {
        int old = s_gain_idx;
        if (luma < 90) s_gain_idx += 2;
        else if (luma > 150) s_gain_idx -= 2;
        if (s_gain_idx < s_gain_min) s_gain_idx = s_gain_min;
        if (s_gain_idx > s_gain_max) s_gain_idx = s_gain_max;
        if (s_gain_idx != old) {
            cam_3a_set_gain(fd);
            ESP_LOGI(TAG, "AE: luma=%d gain idx %d→%d", luma, old, s_gain_idx);
        }
    }

}

// task 统一收尾: 置状态 + 通知 stop 等待者 + 自删 (调用后不得再碰栈上变量;
// vTaskDelete(NULL) 不返回)
static void cam_task_finish()
{
    camera_mutex.lock();
    is_camera_capturing = false;
    camera_mutex.unlock();
    s_photo_req = false;
    camera_task = NULL;
    if (cam_exit_done) xSemaphoreGive(cam_exit_done);
    vTaskDelete(NULL);
}

void app_camera_display(void* arg)
{
    /* camera config */
    static esp_video_init_csi_config_t csi_config = {
        .sccb_config =
            {
                .init_sccb  = false,
                .i2c_handle = NULL,
                .freq       = 400000,  // TAB5_MIPI_CSI_SCCB_I2C_FREQ,
            },
        .reset_pin = -1,  // TAB5_MIPI_CSI_CAM_SENSOR_RESET_PIN,
        .pwdn_pin  = -1,  // TAB5_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
    };
    csi_config.sccb_config.i2c_handle = bsp_i2c_get_handle();

    esp_video_init_config_t cam_config = {
        .csi  = &csi_config,  // Point to CSI config
        .dvp  = NULL,         // No DVP configuration
        .jpeg = NULL,         // No JPEG configuration
    };

    if (!cam_is_initial) {
        esp_err_t err = esp_video_init(&cam_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
            cam_task_finish();
            return;
        }
        int video_cam_fd = app_video_open(CAM_DEV_PATH, EXAMPLE_VIDEO_FMT_RGB565);
        if (video_cam_fd < 0) {
            ESP_LOGE(TAG, "video cam open failed");
            cam_task_finish();
            return;
        }
        err = new_cam(video_cam_fd, &camera);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "new_cam failed: %s", esp_err_to_name(err));
            close(video_cam_fd);
            camera = NULL;
            cam_task_finish();
            return;
        }
        cam_3a_init(video_cam_fd);   // 软件 AE/AWB (社区三项目实测定论)
        cam_is_initial = true;
    }

    struct v4l2_buffer buf;

    uint16_t screen_width  = 1280;
    uint16_t screen_height = 720;
    uint32_t img_show_size = screen_width * screen_height * 2;
    static image_t* img_show;   // 生命周期: task 间复用, 退出释放 (原实现同款)
    if (img_show == NULL) {
        img_show          = (image_t*)calloc(1, sizeof(image_t));
        img_show->w       = 720;
        img_show->h       = 1280;
        img_show->bpp     = 2;   // RGB565 (原未初始化, size 算出垃圾值)
        img_show->pixfmt  = PIXFORMAT_RGB565;
        img_show->size    = img_show->w * img_show->h * img_show->bpp;
        uint8_t* data     = (uint8_t*)heap_caps_calloc(img_show_size, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        if (!data) {
            // 1.8MB DMA+SPIRAM 连续块拿不到 (原实现只打日志继续跑 → PPA 写
            // NULL = StoreProhibited 重启)
            ESP_LOGE(TAG, "img_show alloc failed (%" PRIu32 " bytes)", img_show_size);
            free(img_show);
            img_show = NULL;
            cam_task_finish();
            return;
        }
        img_show->data = data;
    }
    uint8_t* img_show_data = img_show->data;

    ppa_client_handle_t ppa_srm_handle = NULL;
    ppa_client_config_t ppa_srm_config = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t perr = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(perr));
        cam_task_finish();
        return;
    }

    int task_control = 0;
    while (1) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = MEMORY_TYPE;
        const int dq = ioctl(camera->fd, VIDIOC_DQBUF, &buf);
        if (dq == 0) {
            static int s_3a_cnt = 0;
            if (++s_3a_cnt >= 15) {
                s_3a_cnt = 0;
                cam_3a_step(camera->fd, img_show_data, img_show_size);
            }
            ppa_srm_oper_config_t srm_config = {.in             = {.buffer         = camera->buffer[buf.index],
                                                                   .pic_w          = 1280,
                                                                   .pic_h          = 720,
                                                                   .block_w        = 1280,
                                                                   .block_h        = 720,
                                                                   .block_offset_x = 0,
                                                                   .block_offset_y = 0,
                                                                   .srm_cm         = PPA_SRM_COLOR_MODE_RGB565},
                                                .out            = {.buffer         = img_show_data,
                                                                   .buffer_size    = img_show_size,
                                                                   .pic_w          = 1280,
                                                                   .pic_h          = 720,
                                                                   .block_offset_x = 0,
                                                                   .block_offset_y = 0,
                                                                   .srm_cm         = PPA_SRM_COLOR_MODE_RGB565},
                                                .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
                                                .scale_x        = 1,
                                                .scale_y        = 1,
                                                .mirror_x       = true,
                                                .mirror_y       = false,
                                                .rgb_swap       = false,
                                                .byte_swap      = false,
                                                .mode           = PPA_TRANS_MODE_BLOCKING};
            ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);

            // 快门请求: 拷贝当前已 mirror 帧 (RGB565) 给 capturePhotoJpeg 编码
            if (s_photo_req && s_photo_frame) {
                memcpy(s_photo_frame, img_show_data, img_show_size);
                s_photo_req = false;
                xSemaphoreGive(s_photo_sem);
            }

            // 检查+使用都在 LVGL 锁内: stopCameraCapture 在 LVGL 上下文置
            // camera_canvas=NULL 同样持锁 → 与此处互斥, 消 TOCTOU (悬死 task
            // 醒来时 canvas 已 NULL, 安全跳过)
            bsp_display_lock(0);
            if (camera_canvas) {
                lv_canvas_set_buffer(camera_canvas, img_show_data, CAMERA_WIDTH, CAMERA_HEIGHT, LV_COLOR_FORMAT_RGB565);
            }
            bsp_display_unlock();

            if (ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "failed to free video frame");
                break;
            }
        } else {
            // 非阻塞无帧 (EAGAIN): 小歇再试, 不 break (断流≠致命)
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (xQueueReceive(queue_camera_ctrl, &task_control, 0) == pdPASS) {
            if (task_control == TASK_CONTROL_PAUSE) {
                ESP_LOGI(TAG, "task pause");
                if (xQueueReceive(queue_camera_ctrl, &task_control, portMAX_DELAY) == pdPASS) {
                    if (task_control == TASK_CONTROL_EXIT) {
                        break;
                    } else {
                        ESP_LOGI(TAG, "task resume");
                    }
                }
            }
        }

        if (dq == 0) vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "task exit");
    ppa_unregister_client(ppa_srm_handle);
    if (img_show_data) {
        heap_caps_free(img_show_data);
        img_show_data = NULL;
    }
    if (img_show) {
        heap_caps_free(img_show);
        img_show = NULL;
    }
    cam_task_finish();
}

void HalEsp32::startCameraCapture(lv_obj_t* imgCanvas)
{
    mclog::tagInfo(TAG, "start camera capture");
    // camera_task 或旧 task 尚在收尾 (stop 超时窗口) 都要拒 —— 双 task 并发
    // DQBUF 同一 fd 会乱
    if (camera_task || isCameraCapturing()) {
        mclog::tagWarn(TAG, "camera task already running, stop first");
        return;
    }

    camera_canvas = imgCanvas;

    if (queue_camera_ctrl == NULL) queue_camera_ctrl = xQueueCreate(10, sizeof(int));
    if (cam_exit_done == NULL) cam_exit_done = xSemaphoreCreateBinary();
    if (queue_camera_ctrl == NULL || cam_exit_done == NULL) {
        mclog::tagWarn(TAG, "queue/sem create failed");
        return;
    }
    xQueueReset(queue_camera_ctrl);
    xSemaphoreTake(cam_exit_done, 0);   // 清残留

    camera_mutex.lock();
    is_camera_capturing = true;
    camera_mutex.unlock();
    // 16KB 栈: esp_video_init→sensor probe 调用链深, 8KB 有溢出风险
    if (xTaskCreatePinnedToCore(app_camera_display, "cam", 16 * 1024, NULL, 5, &camera_task, 1) != pdPASS) {
        mclog::tagWarn(TAG, "camera task create failed");
        camera_task = NULL;
        camera_mutex.lock();
        is_camera_capturing = false;
        camera_mutex.unlock();
    }
}

void HalEsp32::stopCameraCapture()
{
    mclog::tagInfo(TAG, "stop camera capture");
    if (camera_task == NULL && !isCameraCapturing()) return;   // 没在跑

    // 先置 NULL (本函数总在 LVGL 上下文被调 → 持锁, 与 task 锁内的
    // set_buffer 互斥): 之后 task 醒来直接跳过刷屏, 快速回到 queue 检查。
    camera_canvas = NULL;

    int control_state = 0;  // pause
    xQueueSend(queue_camera_ctrl, &control_state, portMAX_DELAY);

    control_state = 2;  // exit
    xQueueSend(queue_camera_ctrl, &control_state, portMAX_DELAY);

    // 同步等 task 退出。300ms: 若调用方持 LVGL 锁 (按钮回调) 而 task 正卡在
    // set_buffer 等锁, 这里等不满 —— 超时返回后锁释放, task 醒来见 canvas
    // NULL 安全跳过再自行退出 (cam_task_finish 兜底清状态)。勿等太久, 持锁
    // 等待= UI 卡死。
    if (cam_exit_done && xSemaphoreTake(cam_exit_done, pdMS_TO_TICKS(300)) != pdTRUE) {
        mclog::tagWarn(TAG, "camera task exit wait timeout (task may be stuck)");
    }
    camera_task = NULL;
    // is_camera_capturing 由 task finish 清; 超时窗口内它仍 true ——
    // startCameraCapture 的守卫靠它兜住, 防双 task 并发 DQBUF。
}

bool HalEsp32::isCameraCapturing()
{
    std::lock_guard<std::mutex> lock(camera_mutex);
    return is_camera_capturing;
}

std::string HalEsp32::capturePhotoFrame()
{
    if (!isCameraCapturing()) {
        mclog::tagWarn(TAG, "capturePhotoFrame: preview not running");
        return "";
    }
    constexpr size_t FRAME_SIZE = (size_t)CAMERA_WIDTH * CAMERA_HEIGHT * 2;
    if (!s_photo_frame) {
        s_photo_frame = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_photo_frame) {
            mclog::tagWarn(TAG, "photo frame alloc failed");
            return "";
        }
    }
    if (s_photo_sem == NULL) {
        s_photo_sem = xSemaphoreCreateBinary();
        if (s_photo_sem == NULL) return "";
    }
    // 清掉可能残留的旧信号量, 再发请求 (预览 ~30fps, 等一帧 <50ms;
    // task 的快门拷贝在 PPA 后、LVGL 锁外 → 持 LVGL 锁的主线程调用不互锁)
    xSemaphoreTake(s_photo_sem, 0);
    s_photo_req = true;
    if (xSemaphoreTake(s_photo_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        mclog::tagWarn(TAG, "capturePhotoFrame: frame wait timeout");
        s_photo_req = false;
        return "";
    }
    return std::string((char*)s_photo_frame, FRAME_SIZE);
}

std::string HalEsp32::jpegEncodeFrame(const std::string& frame, int quality)
{
    constexpr size_t FRAME_SIZE = (size_t)CAMERA_WIDTH * CAMERA_HEIGHT * 2;
    if (frame.size() != FRAME_SIZE) return "";
    uint8_t* out     = nullptr;
    size_t   out_len = 0;
    if (!image_to_jpeg((uint8_t*)frame.data(), frame.size(), CAMERA_WIDTH, CAMERA_HEIGHT,
                       V4L2_PIX_FMT_RGB565, (uint8_t)quality, &out, &out_len) ||
        !out || out_len == 0) {
        mclog::tagWarn(TAG, "jpeg encode failed");
        if (out) free(out);
        return "";
    }
    mclog::tagInfo(TAG, "photo jpeg: {} bytes", out_len);
    std::string jpeg((char*)out, out_len);
    free(out);
    return jpeg;
}

std::string HalEsp32::capturePhotoJpeg(int quality)
{
    // 兼容单段式 (内部两段; 仍不建议在 LVGL 上下文用 —— 编码 ~1s 会卡
    // 预览 task 的 lv_canvas_set_buffer 等 LVGL 锁)
    std::string frame = capturePhotoFrame();
    if (frame.empty()) return "";
    return jpegEncodeFrame(frame, quality);
}
