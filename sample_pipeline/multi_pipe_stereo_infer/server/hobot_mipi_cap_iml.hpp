#ifndef HOBOT_MIPI_CAP_IML_HPP_
#define HOBOT_MIPI_CAP_IML_HPP_


typedef struct eeprom_id {
	int i2c_bus;           // sensor挂在哪条总线上
	int i2c_dev_addr;      // sensor i2c设备地址
	int i2c_addr_width;    // 总线地址宽（1/2字节）
	int det_reg;           // 读取的寄存器地址
	int check_value;           // 读取的寄存器地址
	char device_name[25];  // sensor名字
} EEPROM_ID_T;

typedef struct eeprom_detect {
	int i2c_bus;           // sensor挂在哪条总线上
	int i2c_dev_addr;      // sensor i2c设备地址
	int i2c_addr_width;    // 总线地址宽（1/2字节）
	int det_reg;           // 读取的寄存器地址
	char check_str[10];           // 读取的寄存器地址
	char device_name[25];  // sensor名字
} EEPROM_DETECT_T;

#pragma pack(4)
typedef struct eeprom_drobot_head_st {
	char flag[8];
	char camType; //0x00:单目；0x01:双目
	char cal_tpye; //0x00:针孔标定；0x01:鱼眼标定
	char ver_main;
	char ver_min;
	char angle; //描述标定时是否旋转后再标定，旋转角度。0x00:表示不旋转，0x01:表示顺时针旋转90度,0x02:表示顺时针旋转180度,0x03:表示顺时针旋转270度,其他的数值无效，表示不旋转。
	char d_num; //D畸变参数的个数，鱼眼标定：k1,k2,k3,k4;针孔标定:k1,k2,p1,p2,k3,k4,k5,k6
	char res2;
	char check;
} EepromDrobotHead_ST;
#pragma pack()

#pragma pack(4)
typedef struct cal_dualcam_info_st {
	double fxl;
	double fyl;
	double cxl;
	double cyl;
	double k1l;
	double k2l;
	double k3l;
	double k4l;
	double k5l;
	double k6l;
	double p1l;
	double p2l;
	double rmsl;
	double fxr;
	double fyr;
	double cxr;
	double cyr;
	double k1r;
	double k2r;
	double k3r;
	double k4r;
	double k5r;
	double k6r;
	double p1r;
	double p2r;
	double rmsr;
	double r11;
	double r12;
	double r13;
	double r21;
	double r22;
	double r23;
	double r31;
	double r32;
	double r33;
	double tx;
	double ty;
	double tz;
	double epilines;
	char h_v[4];
} CalDualCamInfo_ST;
#pragma pack()

#pragma pack(4)
typedef struct cal_dual_M_D_st {
	int width;
	int height;
	float fx;
	float fy;
	float cx;
	float cy;
	float d[8];//鱼眼:k1,k2,k3,k4;针孔:k1,k2,p1,p2,k3,k4,k5,k6
} CalDualMDInfo_ST;
#pragma pack()

#pragma pack(4)
typedef struct cal_dual_R_T_info_st {
	float r11;
	float r12;
	float r13;
	float r21;
	float r22;
	float r23;
	float r31;
	float r32;
	float r33;
	float tx;
	float ty;
	float tz;
} CalDualRTInfo_ST;
#pragma pack()


#endif  // HOBOT_MIPI_CAP_IML_HPP_
