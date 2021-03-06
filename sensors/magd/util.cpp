/*
* Copyright (c) 2008 The Android Open Source Project
* Copyright (c) 2012 Freescale Semiconductor, Inc. All rights reserved.
*
* This software program is proprietary program of Freescale Semiconductor
* licensed to authorized Licensee under Software License Agreement (SLA)
* executed between the Licensee and Freescale Semiconductor.
* 
* Use of the software by unauthorized third party, or use of the software 
* beyond the scope of the SLA is strictly prohibited.
* 
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <poll.h>
#include <linux/input.h>
#include <errno.h>
#include <dirent.h>
#define LOG_TAG "MagDaemon"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "util.h"

#ifndef LOGD
#define LOGD ALOGD
#endif

#ifndef LOGE
#define LOGE ALOGE
#endif
static struct pollfd pollFds[SENSORS_NUM];
static int ori_fd;
static int openInput(const char *inputName,int permission)
{
	int fd = -1;
	const char *dirname = "/dev/input";
	char devname[PATH_MAX];
	char *filename;
	int rw_flag;
	DIR *dir;
	struct dirent *de;
    if(permission == INPUT_DEV_READ_ONLY)
		rw_flag = O_RDONLY;
	else
		rw_flag = O_WRONLY;
	dir = opendir(dirname);
	if (dir == NULL)
		return -1;
	strcpy(devname, dirname);
	filename = devname + strlen(devname);
	*filename++ = '/';
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;
		strcpy(filename, de->d_name);
		fd = open(devname, rw_flag);
		if (fd >= 0) {
			char name[80];
			if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
				name[0] = '\0';
			}
			if (!strcmp(name, inputName)) {
				LOGD("find sensor name %s\n",inputName);
				break;
			} else {
				close(fd);
				fd = -1;
			}
		}
	}
	closedir(dir);
	if (fd < 0)
		LOGD("couldn't find '%s' input device", inputName);
        else
		LOGD("input device '%s' opened, fd = %d\n", inputName, fd);
	return fd;
}


int init_sensors(const char *acc_name, const char *mag_name)
{
	int ret, mode;

	LOGD("init_sensors\n");
	/* Open input device of acclerometer and mag raw data */
	pollFds[SENSOR_ACCEL].fd = openInput(acc_name,INPUT_DEV_READ_ONLY);
	if (pollFds[SENSOR_ACCEL].fd < 0)
		return -1;
	pollFds[SENSOR_MAG].fd = openInput(mag_name,INPUT_DEV_READ_ONLY);
	if (pollFds[SENSOR_MAG].fd < 0) {
		close(pollFds[SENSOR_ACCEL].fd);
		return -1;
	}
	pollFds[SENSOR_ACCEL].events = POLLIN | POLLPRI;
	pollFds[SENSOR_MAG].events = POLLIN | POLLPRI;

	/* create user level input device to trigger calibrated
           mag data nd orientation data */
	ori_fd = openInput(ECOMPASS_INPUT_NAME,INPUT_DEV_WRITED);
	if (ori_fd < 0) {
		close(pollFds[SENSOR_ACCEL].fd);
		close(pollFds[SENSOR_MAG].fd);
		return -1;
	}
	return 0;
}

void deinit_sensors()
{

	/* Close devices */
	if (pollFds[SENSOR_ACCEL].fd)
		close(pollFds[SENSOR_ACCEL].fd);
	if (pollFds[SENSOR_MAG].fd)
		close(pollFds[SENSOR_MAG].fd);
	if (ori_fd)
		close(ori_fd);
}

static void internal_read_abs(int poll_id, short *x, short *y, short *z)
{
	int bytes_read;
	struct input_event event;
	if ((pollFds[poll_id].revents & POLLIN) == POLLIN) {
		while ((bytes_read = read(pollFds[poll_id].fd, &event, sizeof(event))) > 0) {
			if (bytes_read != sizeof(event)) {
				LOGD("byte read %d\n", bytes_read);
				return ;
			}
			if (event.type == EV_ABS) {
				switch (event.code) {
				case ABS_X:
					*x = event.value;
					break;
				case ABS_Y:
					*y = event.value;
					break;
				case ABS_Z:
					*z = event.value;;
					break;
				}
			} else if (event.type == EV_SYN) {
				pollFds[poll_id].revents = 0;
				return;
			}
		}
	}
	return ;
}

int read_sensor_data(short *pAccX, short *pAccY, short *pAccZ, short *pMagX,
		     short *pMagY, short *pMagZ)
{
	int ret, bytes_read;
	struct input_event event;

	ret = poll(&pollFds[SENSOR_MAG], (unsigned long)1, -1);
	if (ret <= 0) {
		//LOGD("poll..: %s\n", strerror(errno));
		return -1;
	}
	internal_read_abs(SENSOR_MAG, pMagX, pMagY, pMagZ);
	
    ret = poll(&pollFds[SENSOR_ACCEL], (unsigned long)1, -1);
	if (ret <= 0) {
		//LOGD("poll..: %s\n", strerror(errno));
		return -1;
	}
	internal_read_abs(SENSOR_ACCEL, pAccX, pAccY, pAccZ);
	return 0;
}

static void internal_inject_event(int fd, int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(struct input_event));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(struct input_event)) < 0)
		LOGE("write error\n");
}

void inject_calibrated_data(int *iBfx, int *iBfy, int *iBfz,
			    int *pLPPsi, int *pLPThe,int *pLPPhi,int status)
{
	int ret, fd;
	int frmdata[6];

	// 16 is the parameter in HAL
	frmdata[0] = *iBfx;
	frmdata[1] = *iBfy;
	frmdata[2] = *iBfz;
	frmdata[3] = *pLPPsi;	//rot around z (yaw)
	frmdata[4] = *pLPThe;	//rot around x (pitch)
	frmdata[5] = *pLPPhi;	//rot around y (roll)

	/* inject the input event to input device(eCompass) */
	fd = ori_fd;
	internal_inject_event(fd, EV_ABS, ABS_X, frmdata[0]);
	internal_inject_event(fd, EV_ABS, ABS_Y, frmdata[1]);
	internal_inject_event(fd, EV_ABS, ABS_Z, frmdata[2]);
	internal_inject_event(fd, EV_ABS, ABS_RX, frmdata[3]);
	internal_inject_event(fd, EV_ABS, ABS_RY, frmdata[4]);
	internal_inject_event(fd, EV_ABS, ABS_RZ, frmdata[5]);
	internal_inject_event(fd, EV_ABS, ABS_STATUS, status);
	internal_inject_event(fd, EV_SYN, SYN_REPORT, 0);

	return;
}
