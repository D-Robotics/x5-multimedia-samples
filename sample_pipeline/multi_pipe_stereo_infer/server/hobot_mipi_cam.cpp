#include <errno.h>
#include <malloc.h>
#include <unistd.h>

#include <assert.h>
#include <fcntl.h> /* low-level i/o */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
// #include <yaml-cpp/yaml.h>

#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <opencv2/opencv.hpp>

#include "hbn_api.h"

#include "hobot_mipi_comm.hpp"
#include "hobot_mipi_cam.hpp"
#include "hobot_mipi_cap_iml.hpp"

#define CAM_LOG(msg) \
    std::cout << __FILE__ << ":" << __LINE__ << " | " << msg << "\n"

typedef struct {
    uint32_t height;
    uint32_t width;
    std::vector<double> d;
    std::array<double, 9> k;
    std::array<double, 9> r;
    std::array<double, 12> p;
} STEREO_CAMERA_INFO_T;

typedef struct gdc_binbuf_s {
    hb_mem_common_buf_t *bin_buf = nullptr;
    uint64_t bin_buf_size;
    ~gdc_binbuf_s() {
        if (bin_buf != NULL) {
            hb_mem_free_buf(bin_buf->fd);
            bin_buf = NULL;
        }
    }
}GdcBinBuf_ST;


bool getDualCamCalibrationIml(STEREO_CAMERA_INFO_T &cam_info_l, STEREO_CAMERA_INFO_T &cam_info_r,
                                  const std::string &file_path) {
//   RCLCPP_WARN(rclcpp::get_logger("mipi_cam"), "cal_file:%s", file_path.c_str());

    std::cout << "============== Get Calibration From File - " << file_path.c_str() << "Bgn: ============ " << std::endl;

    try {
        if ((file_path.length() == 0) || (file_path == "default")) {
        return false;
        }
        cv::FileStorage fs(file_path.c_str(), cv::FileStorage::READ);
        if (!fs.isOpened()) {
            std::cerr << "Open Calibration File Failed" << std::endl;
            return false;
        }
        cv::Mat l_k, l_d, r_k, r_d, R, T;

        int width = fs["image_width"];
        int height = fs["image_height"];
        fs["left_camera_matrix"] >> l_k;
        fs["left_distortion_coefficients"] >> l_d;
        fs["right_camera_matrix"] >> r_k;
        fs["right_distortion_coefficients"] >> r_d;
        fs["R"] >> R;
        fs["T"] >> T;
        fs.release();
        // 检查数据类型并进行转换（如果需要）
        if (l_k.type() != CV_64F) {
        l_k.convertTo(l_k, CV_64F); // 转换为double类型
        }
        if (l_d.type() != CV_64F) {
        l_d.convertTo(l_d, CV_64F); // 转换为double类型
        }
        if (r_k.type() != CV_64F) {
        r_k.convertTo(r_k, CV_64F); // 转换为double类型
        }
        if (r_d.type() != CV_64F) {
        r_d.convertTo(r_d, CV_64F); // 转换为double类型
        }
        if (R.type() != CV_64F) {
        R.convertTo(R, CV_64F); // 转换为double类型
        }
        if (T.type() != CV_64F) {
        T.convertTo(T, CV_64F); // 转换为double类型
        }

        cam_info_r.width = cam_info_l.width = width;
        cam_info_r.height = cam_info_l.height = height;

        cam_info_l.d.resize(l_d.total());
        std::copy(l_d.ptr<double>(0), l_d.ptr<double>(0) + l_d.total(), cam_info_l.d.begin());
        std::copy(l_k.ptr<double>(0), l_k.ptr<double>(0) + l_k.total(), cam_info_l.k.begin());

        cam_info_r.d.resize(r_d.total());
        std::copy(r_d.ptr<double>(0), r_d.ptr<double>(0) + r_d.total(), cam_info_r.d.begin());
        std::copy(r_k.ptr<double>(0), r_k.ptr<double>(0) + r_k.total(), cam_info_r.k.begin());

        cv::Mat l_r_eye = cv::Mat::eye(3, 3, CV_64F);
        std::copy(l_r_eye.ptr<double>(0), l_r_eye.ptr<double>(0) + l_r_eye.total(), cam_info_l.r.begin());

        cv::Mat l_p_eye = cv::Mat::eye(3, 4, CV_64F);
        cv::Mat l_p = l_k * l_p_eye;
        std::copy(l_p.ptr<double>(0), l_p.ptr<double>(0) + l_p.total(), cam_info_l.p.begin());

        cv::Mat RT = cv::Mat::zeros(3, 4, CV_64F);
        R.copyTo(RT(cv::Rect(0, 0, 3, 3)));
        T.reshape(1).copyTo(RT.col(3));
        cv::Mat P = r_k * RT;
        std::copy(R.ptr<double>(0), R.ptr<double>(0) + R.total(), cam_info_r.r.begin());
        std::copy(P.ptr<double>(0), P.ptr<double>(0) + P.total(), cam_info_r.p.begin());
        fs.release();

        std::cout << "getDualCamCalibrationIml OK." << std::endl;

        return true;
    } catch (cv::Exception &e) {
        std::cout << "Unable to parse camera calibration file normally" << std::endl;
        // RCLCPP_ERROR(rclcpp::get_logger("mipi_cam"),
        //   "Unable to parse camera calibration file normally:%s",
        //   e.what());
        return false;
    }
}


#if 1

double  width_tmp;
double  heigh_tmp;
static int save_gdc_bin = 1;

std::vector<std::shared_ptr<GdcBinBuf_ST>> gen_gdc_bin_stereo(int in_width, int in_height,int out_width, int out_height,
        STEREO_CAMERA_INFO_T *cam_info, double rotation, double cal_rotate,
        hb_CameraIntrinsics *cam_intr, const char *left_gdc_path, const char *right_gdc_path) {

    std::vector<std::shared_ptr<GdcBinBuf_ST>> gdc_bin_buf;
    // if (in_width <= 0 || in_height<= 0 || out_width <= 0 || out_height <= 0 || cam_info.size() != 2) {
    if (in_width <= 0 || in_height<= 0 || out_width <= 0 || out_height <= 0) {
        CAM_LOG("resolution param error");
        return gdc_bin_buf;
    }
    if (!((rotation == 0.0) || (rotation == 90.0) || (rotation == 180.0) || (rotation == 270.0) ||
        (cal_rotate == 0.0) || (cal_rotate == 90.0) || (cal_rotate == 180.0) || (cal_rotate == 270.0))) {
        CAM_LOG("rotate param error");
        return gdc_bin_buf;
    }

    float gdc_width_scale, gdc_height_scale;
    int in_gdc_width, in_gdc_height;

    if ((cal_rotate == 90.0) || (cal_rotate == 270.0)) {
        in_gdc_width = in_height;
        in_gdc_height = in_width;
    } else {
        in_gdc_width = in_width;
        in_gdc_height = in_height;
    }

    double rotation_diff = rotation > cal_rotate ? rotation - cal_rotate : 360 + rotation - cal_rotate;
    int out_gdc_width, out_gdc_height;

    if ((rotation_diff == 90.0) || (rotation_diff == 270.0)) {
        out_gdc_width = out_height;
        out_gdc_height = out_width;
    } else {
        out_gdc_width = out_width;
        out_gdc_height = out_height;
    }

    // cam param
    // cal_cam_info.clear();
    cv::Mat Rl, Rr, Pl, Pr, Q;
    cv::Mat Kl, Kr, Dl, Dr, R_rl, t_rl;
    cv::Mat undistmap1l, undistmap2l, undistmap1r, undistmap2r;
    gdc_width_scale = in_gdc_width / static_cast<float>(cam_info[0].width);
    gdc_height_scale = in_gdc_height / static_cast<float>(cam_info[0].height);

    Dl = cv::Mat(1, cam_info[0].d.size(), CV_64F, cam_info[0].d.data()).clone();
    Kl = cv::Mat(3, 3, CV_64F, cam_info[0].k.data()).clone();
    cv::Mat tRl = cv::Mat(3, 3, CV_64F, cam_info[0].r.data()).clone();
    cv::Mat tPl = cv::Mat(3, 4, CV_64F, cam_info[0].p.data()).clone();

    Dr = cv::Mat(1, cam_info[1].d.size(), CV_64F, cam_info[1].d.data()).clone();
    Kr = cv::Mat(3, 3, CV_64F, cam_info[1].k.data()).clone();
    cv::Mat tRr = cv::Mat(3, 3, CV_64F, cam_info[1].r.data()).clone();
    cv::Mat tPr = cv::Mat(3, 4, CV_64F, cam_info[1].p.data()).clone();

    R_rl = cv::Mat::zeros(3, 3, CV_64F);
    t_rl = cv::Mat::zeros(3, 1, CV_64F);

    cv::Mat Kr_inv = Kr.inv();
    cv::Mat RT = Kr_inv * tPr;
    cv::Mat tTr = RT(cv::Rect(3, 0, 1, 3));
    R_rl = tRr;
    t_rl = tTr;

    Kl.at<double>(0, 0) *= gdc_width_scale;
    Kl.at<double>(0, 2) *= gdc_width_scale;
    Kl.at<double>(1, 1) *= gdc_height_scale;
    Kl.at<double>(1, 2) *= gdc_height_scale;
    Kr.at<double>(0, 0) *= gdc_width_scale;
    Kr.at<double>(0, 2) *= gdc_width_scale;
    Kr.at<double>(1, 1) *= gdc_height_scale;
    Kr.at<double>(1, 2) *= gdc_height_scale;
    std::cout << "gdc_width_scale"<<gdc_width_scale << std::endl;
    std::cout << "gdc_height_scale" <<gdc_height_scale << std::endl;
    std::cout << "Kl:\n" << Kl << std::endl;
    std::cout << "Dl:\n" << Dl << std::endl;
    std::cout << "Kr:\n" << Kr << std::endl;
    std::cout << "Dr:\n" << Dr << std::endl;
    std::cout << "R_rl:\n" << R_rl << std::endl;
    std::cout << "t_rl:\n" << t_rl << std::endl;
    cv::stereoRectify(Kl, Dl, Kr, Dr, cv::Size(in_gdc_width, in_gdc_height), R_rl, t_rl, Rl, Rr, Pl, Pr, Q, cv::CALIB_ZERO_DISPARITY, 0 ,cv::Size(out_gdc_width, out_gdc_height));
    cv::initUndistortRectifyMap(Kl, Dl, Rl, Pl, cv::Size(out_gdc_width, out_gdc_height), CV_32FC1, undistmap1l, undistmap2l);
    cv::initUndistortRectifyMap(Kr, Dr, Rr, Pr, cv::Size(out_gdc_width, out_gdc_height), CV_32FC1, undistmap1r, undistmap2r);
    int rotation_diff_int = rotation_diff;
    cv::Mat tmp;
    cv::Mat rotation_1l;
    cv::Mat rotation_2l;
    cv::Mat rotation_1r;
    cv::Mat rotation_2r;
    switch(rotation_diff_int) {	
        case 90:
            cv::transpose(undistmap1l, tmp);
            cv::flip(tmp, rotation_1l, 1); // 垂直翻转
            cv::transpose(undistmap2l, tmp);
            cv::flip(tmp, rotation_2l, 1); // 垂直翻转

            cv::transpose(undistmap1r, tmp);
            cv::flip(tmp, rotation_1r, 1); // 垂直翻转
            cv::transpose(undistmap2r, tmp);
            cv::flip(tmp, rotation_2r, 1); // 垂直翻转
            break;
        case 180:
            cv::flip(undistmap1l, rotation_1l, -1);
            cv::flip(undistmap2l, rotation_2l, -1);

            cv::flip(undistmap1r, rotation_1r, -1);
            cv::flip(undistmap2r, rotation_2r, -1);
            break;
        case 270:
            cv::transpose(undistmap1l, tmp);
            cv::flip(tmp, rotation_1l, 0); // 垂直翻转
            cv::transpose(undistmap2l, tmp);
            cv::flip(tmp, rotation_2l, 0); // 垂直翻转

            cv::transpose(undistmap1r, tmp);
            cv::flip(tmp, rotation_1r, 0); // 垂直翻转
            cv::transpose(undistmap2r, tmp);
            cv::flip(tmp, rotation_2r, 0); // 垂直翻转
            break;
        default:
            rotation_1l = undistmap1l;
            rotation_2l = undistmap2l;

            rotation_1r = undistmap1r;
            rotation_2r = undistmap2r;
            break;
    }

    std::cout << "Rl:\n" << Rl << std::endl;
    std::cout << "Rr:\n" << Rr << std::endl;
    std::cout << "Pl:\n" << Pl << std::endl;
    std::cout << "Pr:\n" << Pr << std::endl;

    param_t gdc_param;
    memset(&gdc_param, 0, sizeof(param_t));
    gdc_param.format = FMT_SEMIPLANAR_420;
    gdc_param.in.w = in_width;
    gdc_param.in.h = in_height;
    gdc_param.out.w = out_width;
    gdc_param.out.h = out_height;
    gdc_param.x_offset = 0;
    gdc_param.y_offset = 0;
    gdc_param.diameter = in_height;
    gdc_param.fov = 180;

    window_t  wnds;
    memset(&wnds, 0, sizeof(window_t));
    wnds.strength = 1.0;
    wnds.strengthY = 1.0;
    wnds.angle = rotation;
    //wnds.angle = 0;
    wnds.elevation = 0;
    wnds.azimuth = 0;
    wnds.keep_ratio = 1;
    wnds.FOV_h = 90;
    wnds.FOV_w = 90;
    wnds.cylindricity_y = 0;
    wnds.cylindricity_x = 0;
    wnds.trapezoid_left_angle = 90;
    wnds.trapezoid_right_angle = 90;

    wnds.out_r.x = 0;
    wnds.out_r.y = 0;
    wnds.out_r.w = out_width;
    wnds.out_r.h = out_height;
    wnds.input_roi_r.x = 0;
    wnds.input_roi_r.y = 0;
    wnds.input_roi_r.w = in_width;
    wnds.input_roi_r.h = in_height;
    wnds.pan = 0;
    wnds.tilt = 0;
    wnds.zoom = 1;

    wnds.transform = CUSTOM;
    wnds.custom.full_tile_calc = 1;
    wnds.custom.tile_incr_x = 50;
    wnds.custom.tile_incr_y = 50;
    wnds.custom.w = out_width-1;
    wnds.custom.h = out_height-1;
    wnds.custom.centerx = out_width / 2 - 1;
    wnds.custom.centery = out_height / 2 - 1;

    std::vector<point_t> bin_map(out_width * out_height);
    width_tmp = in_width;
    heigh_tmp = in_height;
    int cal_rotate_int = cal_rotate;
    switch(cal_rotate_int) {
        case 90:
            std::transform(rotation_1l.ptr<float>(), rotation_1l.ptr<float>() + rotation_1l.total(),
                rotation_2l.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = static_cast<double>(y);
                    p.y = heigh_tmp - static_cast<double>(x)-1;
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        case 180:
            std::transform(rotation_1l.ptr<float>(), rotation_1l.ptr<float>() + rotation_1l.total(),
                rotation_2l.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = width_tmp - static_cast<double>(x)-1;
                    p.y = heigh_tmp - static_cast<double>(y)-1;
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        case 270:
            std::transform(rotation_1l.ptr<float>(), rotation_1l.ptr<float>() + rotation_1l.total(),
                rotation_2l.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = width_tmp - static_cast<double>(y)-1;
                    p.y = static_cast<double>(x);
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        default:
            std::transform(rotation_1l.ptr<float>(), rotation_1l.ptr<float>() + rotation_1l.total(),
            rotation_2l.ptr<float>(), bin_map.begin(),
            [](float x, float y) {
                point_t p;
                p.x = static_cast<double>(x<0?0:x);
                p.y = static_cast<double>(y<0?0:y);
                return p;
            });
            break;
    }
    wnds.custom.points = bin_map.data();
    uint32_t *bin_buf_ptr = nullptr;
    uint64_t bin_buf_size;
    int64_t alloc_flags = 0;
    int offset = 0;
    auto ret = hbn_gen_gdc_bin(&gdc_param, &wnds, 1, (uint32_t**)&bin_buf_ptr, &bin_buf_size);
    if (ret != 0 || bin_buf_ptr == nullptr) {
        // RCLCPP_ERROR(rclcpp::get_logger("mipi_cap"),"hbn_gen_gdc_bin failed, ret = %d\n", ret);
        std::cout << "hbn_gen_gdc_bin for bin fail, ret = " << ret << std::endl;
        return gdc_bin_buf;
    }

    std::cout << "left_camera_gdc---bin_buf_size:" << bin_buf_size << std::endl;
    if (save_gdc_bin) {
        std::ofstream outfile_;
        if (!outfile_.is_open()) {
            // outfile_.open("./left_camera_gdc.bin", std::ios::app | std::ios::out | std::ios::binary);
            outfile_.open(left_gdc_path, std::ios::app | std::ios::out | std::ios::binary);
        }
        if (outfile_.is_open()) {
            outfile_.write(reinterpret_cast<char *>(bin_buf_ptr), bin_buf_size);
        }
        outfile_.close();
    }
    hb_mem_common_buf_t *bin_buf = new hb_mem_common_buf_t;
    memset(bin_buf, 0, sizeof(hb_mem_common_buf_t));
    alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED | HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD | HB_MEM_USAGE_CPU_READ_OFTEN |
                HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
    ret = hb_mem_alloc_com_buf(bin_buf_size, alloc_flags, bin_buf);
    if (ret != 0 || bin_buf->virt_addr == NULL) {
        hbn_free_gdc_bin(bin_buf_ptr);
        // RCLCPP_ERROR(rclcpp::get_logger("mipi_cap"),"hb_mem_alloc_com_buf for bin failed, ret = %d\n", ret);
        CAM_LOG("hb_mem_alloc_com_buf failed");
        return gdc_bin_buf;
    }
    memcpy(bin_buf->virt_addr, bin_buf_ptr, bin_buf_size);
    ret = hb_mem_flush_buf(bin_buf->fd, offset, bin_buf_size);
    if (ret != 0 || bin_buf->virt_addr == NULL) {
        hbn_free_gdc_bin(bin_buf_ptr);
        // RCLCPP_ERROR(rclcpp::get_logger("mipi_cap"),"hb_mem_flush_buf for bin failed, ret = %d\n", ret);
        CAM_LOG("hb_mem_flush_buf failed");
        return gdc_bin_buf;
    }
    hbn_free_gdc_bin(bin_buf_ptr);
    auto gdc_bin_ptr = std::make_shared<GdcBinBuf_ST>();
    gdc_bin_ptr->bin_buf = bin_buf;
    gdc_bin_ptr->bin_buf_size = bin_buf_size;
    gdc_bin_buf.push_back(gdc_bin_ptr);


    switch(cal_rotate_int) {
        case 90:
            std::transform(rotation_1r.ptr<float>(), rotation_1r.ptr<float>() + rotation_1r.total(),
                rotation_2r.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = static_cast<double>(y);
                    p.y = heigh_tmp - static_cast<double>(x)-1;
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        case 180:
            std::transform(rotation_1r.ptr<float>(), rotation_1r.ptr<float>() + rotation_1r.total(),
                rotation_2r.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = width_tmp - static_cast<double>(x)-1;
                    p.y = heigh_tmp - static_cast<double>(y)-1;
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        case 270:
            std::transform(rotation_1r.ptr<float>(), rotation_1r.ptr<float>() + rotation_1r.total(),
                rotation_2r.ptr<float>(), bin_map.begin(),
                [](float x, float y) {
                    point_t p;
                    p.x = width_tmp - static_cast<double>(y)-1;
                    p.y = static_cast<double>(x);
                    p.x = p.x<0?0:p.x;
                    p.y = p.y<0?0:p.y;
                    return p;
                });
            break;
        default:
            std::transform(rotation_1r.ptr<float>(), rotation_1r.ptr<float>() + rotation_1r.total(),
            rotation_2r.ptr<float>(), bin_map.begin(),
            [](float x, float y) {
                point_t p;
                p.x = static_cast<double>(x<0?0:x);
                p.y = static_cast<double>(y<0?0:y);
                return p;
            });
            break;
    }

    wnds.custom.points = bin_map.data();
    bin_buf_ptr = nullptr;
    ret = hbn_gen_gdc_bin(&gdc_param, &wnds, 1, (uint32_t**)&bin_buf_ptr, &bin_buf_size);
    if (ret != 0 || bin_buf_ptr == nullptr) {
        // RCLCPP_ERROR(rclcpp::get_logger("mipi_cap"),"hbn_gen_gdc_bin failed, ret = %d\n", ret);
        CAM_LOG("hbn_gen_gdc_bin failed");
        return gdc_bin_buf;
    }
    std::cout << "right_camera_gdc---bin_buf_size:" << bin_buf_size << std::endl;
    if (save_gdc_bin) {
        std::ofstream outfile_;
        if (!outfile_.is_open()) {
            // outfile_.open("./right_camera_gdc.bin", std::ios::app | std::ios::out | std::ios::binary);
            outfile_.open(right_gdc_path, std::ios::app | std::ios::out | std::ios::binary);
        }
        if (outfile_.is_open()) {
            outfile_.write(reinterpret_cast<char *>(bin_buf_ptr), bin_buf_size);
        }
        outfile_.close();
    }
    bin_buf = new hb_mem_common_buf_t;
    memset(bin_buf, 0, sizeof(hb_mem_common_buf_t));
    alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED | HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD | HB_MEM_USAGE_CPU_READ_OFTEN |
                HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
    ret = hb_mem_alloc_com_buf(bin_buf_size, alloc_flags, bin_buf);
    if (ret != 0 || bin_buf->virt_addr == NULL) {
        hbn_free_gdc_bin(bin_buf_ptr);
        // RCLCPP_INFO(rclcpp::get_logger("mipi_cam"),"hb_mem_alloc_com_buf for bin failed, ret = %d\n", ret);
        CAM_LOG("hb_mem_alloc_com_buf failed");
        return gdc_bin_buf;
    }

    memcpy(bin_buf->virt_addr, bin_buf_ptr, bin_buf_size);
    ret = hb_mem_flush_buf(bin_buf->fd, offset, bin_buf_size);
    if (ret != 0 || bin_buf->virt_addr == NULL) {
        hbn_free_gdc_bin(bin_buf_ptr);
        // RCLCPP_INFO(rclcpp::get_logger("mipi_cam"),"hb_mem_flush_buf for bin failed, ret = %d\n", ret);
        CAM_LOG("hb_mem_flush_buf failed");
        return gdc_bin_buf;
    }
    hbn_free_gdc_bin(bin_buf_ptr);
    gdc_bin_ptr = std::make_shared<GdcBinBuf_ST>();
    gdc_bin_ptr->bin_buf = bin_buf;
    gdc_bin_ptr->bin_buf_size = bin_buf_size;
    gdc_bin_buf.push_back(gdc_bin_ptr);

    float camera_cx, camera_cy, camera_fx, camera_fy, base_line;
    camera_fx = Q.at<double>(2, 3);
    camera_fy = Q.at<double>(2, 3);
    camera_cx = -Q.at<double>(0, 3);
    camera_cy = -Q.at<double>(1, 3);
    base_line = std::abs(1 / Q.at<double>(3, 2));

    // Config Camera Intrinsics
    cam_intr->fx = camera_fx;
    cam_intr->fy = camera_fy;
    cam_intr->cx = camera_cx;
    cam_intr->cy = camera_cy;
    cam_intr->baseline = base_line;

    cv::Mat K = cv::Mat::zeros(3, 3, CV_64F);
    K.at<double>(0, 0) = camera_fx;
    K.at<double>(0, 2) = camera_cx;
    K.at<double>(1, 1) = camera_fy;
    K.at<double>(1, 2) = camera_cy;
    K.at<double>(2, 2) = 1;

    double tmp_t = 0;
    switch(rotation_diff_int) {	
        case 90:
        case 270:
            tmp_t = K.at<double>(0,0);
            K.at<double>(0,0) = K.at<double>(1,1);
            K.at<double>(1,1) = tmp_t;
            tmp_t = K.at<double>(0,2);
            K.at<double>(0,2) = out_height - K.at<double>(1,2);
            K.at<double>(1,2) = tmp_t;
            break;
        default:
            break;
    }

    RT = cv::Mat::eye(3, 4, CV_64F);
    cv::Mat P = K * RT;

    #if 0
    sensor_msgs::msg::CameraInfo tmp_cam_info; 
    tmp_cam_info.width = out_width;
    tmp_cam_info.height = out_height;
    tmp_cam_info.d.resize(5, 0.0);
    memcpy(tmp_cam_info.k.data(), K.data, sizeof(tmp_cam_info.k));

    tmp_cam_info.r[0] = 1.0;
    tmp_cam_info.r[1] = 0.0;
    tmp_cam_info.r[2] = 0.0;
    tmp_cam_info.r[3] = 0.0;
    tmp_cam_info.r[4] = 1.0;
    tmp_cam_info.r[5] = 0.0;
    tmp_cam_info.r[6] = 0.0;
    tmp_cam_info.r[7] = 0.0;
    tmp_cam_info.r[8] = 1.0;

    memcpy(tmp_cam_info.p.data(), P.data, sizeof(tmp_cam_info.p));
    cal_cam_info.push_back(tmp_cam_info);

    RT.at<double>(0, 3) = base_line;
    P = K * RT;
    memcpy(tmp_cam_info.p.data(), P.data, sizeof(tmp_cam_info.p));
    cal_cam_info.push_back(tmp_cam_info);
    #endif

    std::cout << "Kl:" << std::endl
            << Kl << std::endl
            << "Dl:" << std::endl
            << Dl << std::endl
            << "Kr: " << std::endl
            << Kr << std::endl
            << "Dr:" << std::endl
            << Dr << std::endl
            << "R, t: " << std::endl
            << R_rl << std::endl
            << t_rl << std::endl
            << "calib file width, height: " << cam_info[0].width << ", " << cam_info[0].height << std::endl
            << "gdc_width_scale, gdc_height_scale: " << gdc_width_scale << ", " << gdc_height_scale << std::endl
            << "rectify [f, cx, cy, baseline]: " << "[" << camera_fx << ", " << camera_cx << ", " << camera_cy << ", " << base_line << "]" << std::endl
            << std::endl;

    std::cout << "gen_gdc_bin_stereo OK." << std::endl;

    return gdc_bin_buf;
}



bool readEeprom16(uint32_t bus, uint8_t i2c_addr, uint16_t reg_addr, char* buf, int bufsize) {
    int32_t ret;
    struct i2c_rdwr_ioctl_data data;
    uint8_t sendbuf[32] = {0};
    // uint8_t readbuf[32] = {0};  // no used
    struct i2c_msg msgs[I2C_RDRW_IOCTL_MAX_MSGS] = {0};
    char filename[20];
    int file;

    // Open the I2C bus
    snprintf(filename, sizeof(filename), "/dev/i2c-%d", bus);
    file = open(filename, O_RDWR);
    if (file < 0) {
        //std::cout << "Failed to open the I2C bus " << bus << std::endl;
        //perror("open the I2C bus");
        return false;
    }

    sendbuf[0] = (uint8_t)((reg_addr >> 8u) & 0xffu);
    sendbuf[1] = (uint8_t)(reg_addr & 0xffu);

    data.msgs = msgs; /*PRQA S 5118*/
    data.nmsgs = 2;

    data.msgs[0].len = 2;
    data.msgs[0].addr = i2c_addr;
    data.msgs[0].flags = 0;
    data.msgs[0].buf = sendbuf;

    data.msgs[1].len = bufsize;
    data.msgs[1].addr = i2c_addr;
    data.msgs[1].flags = I2C_M_RD;
    data.msgs[1].buf = (uint8_t*)buf;

    ret = ioctl(file, I2C_RDWR, (uint64_t)&data);
    if (ret < 0) {
        // perror("Failed to read from the I2C bus");
        //*value = 0;
        close(file);
        return false;
    }

    //*value = (uint16_t)((readbuf[0] << 8) | readbuf[1]);

    // Close the I2C bus
    close(file);

    return true;
}



bool getDualCamCalibration_yugang(STEREO_CAMERA_INFO_T *cam_info_, int i2c_bus, uint16_t i2c_addr) {
    std::string device;
    std::vector<char> head_buf;
    head_buf.resize(sizeof(EepromDrobotHead_ST));
    char chech_value;
    if (readEeprom16(i2c_bus, i2c_addr, 0x0000, head_buf.data(), sizeof(EepromDrobotHead_ST)) == false) {
        return false;
    }
    int chech_index = sizeof(EepromDrobotHead_ST) - 1;
    chech_value = head_buf[chech_index];
    head_buf[chech_index] = 0;
    int sum = 0;

    std::for_each(head_buf.begin(), head_buf.end(), [&sum](char c) {
        sum += static_cast<int>(c);
    });
    if (((sum % 255) + 1) == chech_value) {
    EepromDrobotHead_ST* head_buf_ptr = (EepromDrobotHead_ST *)head_buf.data();
    std::cout << "====EepromDrobotHead======" << std::endl;
    std::cout << "flag:" << head_buf_ptr->flag << std::endl;
    printf("camType:%d\n", head_buf_ptr->camType);
    printf("cal_tpye:%d\n", head_buf_ptr->cal_tpye);
    printf("ver_main:%d\n", head_buf_ptr->ver_main);
    printf("ver_min:%d\n", head_buf_ptr->ver_min);
    printf("angle:%d\n", head_buf_ptr->angle);
    printf("d_num:%d\n", head_buf_ptr->d_num);

    #if 0   // no used
    if (head_buf_ptr->angle == 0x00) {
        cap_info_.cal_rotation_ = 0.0;
    } else if (head_buf_ptr->angle == 0x01) {
        cap_info_.cal_rotation_ = 90.0;
    } else if (head_buf_ptr->angle == 0x02) {
        cap_info_.cal_rotation_ = 180.0;
    } else if (head_buf_ptr->angle == 0x03) {
        cap_info_.cal_rotation_ = 270.0;
    }

    if (head_buf_ptr->cal_tpye == 0x00) {
        cal_tpye_ = 0; //针孔标定
    } else if (head_buf_ptr->cal_tpye == 0x01) {
        cal_tpye_ = 1; //鱼眼标定
    }
    #endif

    if (head_buf_ptr->camType == 0x01) {
    //		cam_info_.resize(2);
        CalDualMDInfo_ST m_d_info_l, m_d_info_r;
        CalDualRTInfo_ST r_t_info;
        if (readEeprom16(i2c_bus, i2c_addr, 0x0010, (char*)&m_d_info_l, sizeof(CalDualMDInfo_ST)) == false) {
            return false;
        }
        if (readEeprom16(i2c_bus, i2c_addr, 0x0048, (char*)&m_d_info_r, sizeof(CalDualMDInfo_ST)) == false) {
            return false;
        }
        if (readEeprom16(i2c_bus, i2c_addr, 0x008C, (char*)&r_t_info, sizeof(CalDualRTInfo_ST)) == false) {
            return false;
        }

        std::cout << "===========================" << std::endl;
        std::cout << "m_d_info_l" << std::endl;
        printf("width:%d\n",m_d_info_l.width);
        printf("height:%d\n",m_d_info_l.height);
        printf("fx:%f\n",m_d_info_l.fx);
        printf("cx:%f\n",m_d_info_l.cx);
        printf("fy:%f\n",m_d_info_l.fy);
        printf("cy:%f\n",m_d_info_l.cy);

        // printf("k1:%f\n",m_d_info_l.k1);
        // printf("k2:%f\n",m_d_info_l.k2);
        // printf("p1:%f\n",m_d_info_l.p1);
        // printf("p2:%f\n",m_d_info_l.p2);
        // printf("k3:%f\n",m_d_info_l.k3);
        // printf("k4:%f\n",m_d_info_l.k4);
        // printf("k5:%f\n",m_d_info_l.k5);
        // printf("k6:%f\n",m_d_info_l.k6);

        std::cout << "===========================" << std::endl;
        std::cout << "m_d_info_r" << std::endl;
        printf("width:%d\n",m_d_info_r.width);
        printf("height:%d\n",m_d_info_r.height);
        printf("fx:%f\n",m_d_info_r.fx);
        printf("cx:%f\n",m_d_info_r.cx);
        printf("fy:%f\n",m_d_info_r.fy);
        printf("cy:%f\n",m_d_info_r.cy);
        // printf("k1:%f\n",m_d_info_r.k1);
        // printf("k2:%f\n",m_d_info_r.k2);
        // printf("p1:%f\n",m_d_info_r.p1);
        // printf("p2:%f\n",m_d_info_r.p2);
        // printf("k3:%f\n",m_d_info_r.k3);
        // printf("k4:%f\n",m_d_info_r.k4);
        // printf("k5:%f\n",m_d_info_r.k5);
        // printf("k6:%f\n",m_d_info_r.k6);

        std::cout << "===========================" << std::endl;
        std::cout << "r_t_info" << std::endl;
        printf("r11:%f\n",r_t_info.r11);
        printf("r12:%f\n",r_t_info.r12);
        printf("r13:%f\n",r_t_info.r13);
        printf("r21:%f\n",r_t_info.r21);
        printf("r22:%f\n",r_t_info.r22);
        printf("r23:%f\n",r_t_info.r23);
        printf("r31:%f\n",r_t_info.r31);
        printf("r32:%f\n",r_t_info.r32);
        printf("r33:%f\n",r_t_info.r33);
        printf("tx:%f\n",r_t_info.tx);
        printf("ty:%f\n",r_t_info.ty);
        printf("tz:%f\n",r_t_info.tz);

    #if 1   // no used
        cam_info_[0].width = m_d_info_l.width;
        cam_info_[0].height = m_d_info_l.height;
        cam_info_[1].width = m_d_info_r.width;
        cam_info_[1].height = m_d_info_r.height;
    #endif

        cv::Mat l_k= cv::Mat::zeros(3,3,CV_64F);
        l_k.at<double>(0,0) = m_d_info_l.fx;
        l_k.at<double>(0,2) = m_d_info_l.cx;
        l_k.at<double>(1,1) = m_d_info_l.fy;
        l_k.at<double>(1,2) = m_d_info_l.cy;
        l_k.at<double>(2,2) = 1;
        std::copy(l_k.ptr<double>(0), l_k.ptr<double>(0) + l_k.total(), cam_info_[0].k.begin());

        int d_num = 8;
        if (head_buf_ptr->d_num <= 0 && head_buf_ptr->d_num >=4) {
            d_num = head_buf_ptr->d_num;
        }
        cam_info_[0].d.resize(d_num);
        for (int i = 0; i < d_num; i++) {
            cam_info_[0].d[i] = m_d_info_l.d[i];
        }
        // cam_info_[0].d[0] = m_d_info_l.k1;
        // cam_info_[0].d[1] = m_d_info_l.k2;
        // cam_info_[0].d[2] = m_d_info_l.p1;
        // cam_info_[0].d[3] = m_d_info_l.p2;
        // cam_info_[0].d[4] = m_d_info_l.k3;
        // cam_info_[0].d[5] = m_d_info_l.k4;
        // cam_info_[0].d[6] = m_d_info_l.k5;
        // cam_info_[0].d[7] = m_d_info_l.k6;

        cv::Mat l_r_eye = cv::Mat::eye(3, 3, CV_64F);
        std::copy(l_r_eye.ptr<double>(0), l_r_eye.ptr<double>(0) + l_r_eye.total(), cam_info_[0].r.begin());

        cv::Mat l_p_eye = cv::Mat::eye(3, 4, CV_64F);
        cv::Mat l_p = l_k * l_p_eye;
        std::copy(l_p.ptr<double>(0), l_p.ptr<double>(0) + l_p.total(), cam_info_[0].p.begin());



        cv::Mat r_k= cv::Mat::zeros(3,3,CV_64F);
        r_k.at<double>(0,0) = m_d_info_r.fx;
        r_k.at<double>(0,2) = m_d_info_r.cx;
        r_k.at<double>(1,1) = m_d_info_r.fy;
        r_k.at<double>(1,2) = m_d_info_r.cy;
        r_k.at<double>(2,2) = 1;
        std::copy(r_k.ptr<double>(0), r_k.ptr<double>(0) + r_k.total(), cam_info_[1].k.begin());

        // cam_info_[1].d.resize(8);
        // cam_info_[1].d[0] = m_d_info_r.k1;
        // cam_info_[1].d[1] = m_d_info_r.k2;
        // cam_info_[1].d[2] = m_d_info_r.p1;
        // cam_info_[1].d[3] = m_d_info_r.p2;
        // cam_info_[1].d[4] = m_d_info_r.k3;
        // cam_info_[1].d[5] = m_d_info_r.k4;
        // cam_info_[1].d[6] = m_d_info_r.k5;
        // cam_info_[1].d[7] = m_d_info_r.k6;

        cam_info_[1].d.resize(d_num);
        for (int i = 0; i < d_num; i++) {
            cam_info_[1].d[i] = m_d_info_r.d[i];
        }

        cv::Mat R = cv::Mat::zeros(3, 3, CV_64F);
        R.at<double>(0,0) = r_t_info.r11;
        R.at<double>(0,1) = r_t_info.r12;
        R.at<double>(0,2) = r_t_info.r13;
        R.at<double>(1,0) = r_t_info.r21;
        R.at<double>(1,1) = r_t_info.r22;
        R.at<double>(1,2) = r_t_info.r23;
        R.at<double>(2,0) = r_t_info.r31;
        R.at<double>(2,1) = r_t_info.r32;
        R.at<double>(2,2) = r_t_info.r33;

        cv::Mat T = cv::Mat::zeros(3, 1, CV_64F);
        T.at<double>(0,0) = r_t_info.tx;
        T.at<double>(0,1) = r_t_info.ty;
        T.at<double>(0,2) = r_t_info.tz;

        cv::Mat RT;
        cv::hconcat(R, T, RT);
        cv::Mat P = r_k * RT;
        std::copy(R.ptr<double>(0), R.ptr<double>(0) + R.total(), cam_info_[1].r.begin());
        std::copy(P.ptr<double>(0), P.ptr<double>(0) + P.total(), cam_info_[1].p.begin());
        return true;
    }

    }
    return false;
}


int detectEeprom_drobot(std::string &device, int &i2c_bus, uint16_t &i2c_addr) {

    // mipi sensor的信息数组
    EEPROM_DETECT_T eeprom_detect_list[] = {
        {1, 0x50, I2C_ADDR_16, 0x00, "SZYGSJKJ", "yuguang"},  // P24C64G-C4H-MIR
    };
    std::vector<int> i2c_buss= {0,1,2,3,4,5,6,7,8,9,10};

    char buf[9] = {0};
    std::vector<char> buf_type;
    buf_type.resize(0x1f);
    // char check_0;
    // char checksum;
    std::string chip_type;

    for (auto num : i2c_buss) {
        for (auto eeprom_id : eeprom_detect_list) {
            if (readEeprom16(num, eeprom_id.i2c_dev_addr, eeprom_id.det_reg, buf, 8)) {
                std::string buf_str = buf;
                std::cout << "EEPROM FLAG:" << buf_str << std::endl;
                if (eeprom_id.check_str == buf_str) {
                    i2c_bus = num;
                    i2c_addr = eeprom_id.i2c_dev_addr;
                    device = eeprom_id.device_name;
                    return 0;
                }
            }
        }
    }
    return -1;
}

bool getDualCamCalibrationFromEeprom(STEREO_CAMERA_INFO_T *cam_info_) {
    int i2c_bus;
    uint16_t i2c_addr;
    std::string device;
    std::vector<char> i2c_buf;
    i2c_buf.resize(sizeof(CalDualCamInfo_ST));

    std::cout << "============== Get Calibration From EEPROM Bgn: ============ " << std::endl;

    // char chech_value;
    if (detectEeprom_drobot(device, i2c_bus, i2c_addr) == -1) {
        std::cout << "detectEeprom_drobot Failed " << std::endl;
        return false;
    }
    if (device == "yuguang") {
        if (true != getDualCamCalibration_yugang(cam_info_, i2c_bus, i2c_addr)) {
            std::cout << "getDualCamCalibration_yugang Failed " << std::endl;
            return false;
        }
        return true;
    }
    std::cout << "Not Support Module Calibration From EEPROM - " << device << std::endl;
    return false;
}


bool gen_dual_gdc_bin(int calib_type, hb_CameraIntrinsics *cam_intr, const char *left_gdc_path, const char *right_gdc_path)
{
    STEREO_CAMERA_INFO_T cam_info_v[2] = {0};
    // GdcBinBuf_ST gdc_bin_buf;

    if (calib_type == CALIBRATE_IN_EEPROM) {
        // 1. Get Calibration From EEPROM
        if (true != getDualCamCalibrationFromEeprom(cam_info_v)) {
            std::cout << "getDualCamCalibrationFromEeprom Failed." << std::endl;
            return false;
        }
    }
    else if (calib_type == CALIBRATE_IN_FILE){
        // 2. Get Calibration From File
        if (true != getDualCamCalibrationIml(cam_info_v[0], cam_info_v[1], "./config/x5/SC132gs_dual_calibration.yaml")) {
            std::cout << "getDualCamCalibrationIml Failed." << std::endl;
            return false;
        }
    }
    else {
        std::cout << "Not Support Calibration Type: " << calib_type << std::endl;
        return false;
    }

    gen_gdc_bin_stereo(1088, 1280, 1280, 1088, cam_info_v, 90, 90, cam_intr, left_gdc_path, right_gdc_path);

    std::cout << "gen_dual_gdc_bin OK." << std::endl;
    return true;
}

#endif
