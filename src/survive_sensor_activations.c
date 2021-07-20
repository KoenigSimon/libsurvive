#include "string.h"
#include <assert.h>
#include <math.h>
#include <survive.h>

STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_GYRO, "move-threshold-gyro", 'f', "Threshold to count gyro norms as moving", .075)
STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_ACC, "move-threshold-acc", 'f', "Threshold to count acc diff norms as moving", .03)
STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_ANG, "move-threshold-ang", 'f', "Threshold to count light angle diffs as moving",
				   .015)
STATIC_CONFIG_ITEM(FILTER_THRESHOLD_ANG, "filter-threshold-ang-per-sec", 'f',
				   "Threshold to filter light which changes too fast", 50.)

STATIC_CONFIG_ITEM(FILTER_LIGHT_OUTLIER_CRITERIA, "filter-light-outlier-criteria", 'f',
				   "Threshold to filter outlier light strikes", 0.5)

static FLT moveThresholdGyro = 0;
static FLT moveThresholdAcc = 0;
static FLT moveThresholdAng = 0;
static FLT filterLightChange = 0;
static FLT filterOutlierCriteria = 0;

bool SurviveSensorActivations_is_reading_valid(const SurviveSensorActivations *self, survive_long_timecode tolerance,
											   uint32_t sensor_idx, int lh, int axis) {
	return SurviveSensorActivations_time_since_last_reading(self, sensor_idx, lh, axis) <= tolerance;
}

survive_long_timecode SurviveSensorActivations_last_reading(const SurviveSensorActivations *self, uint32_t sensor_idx,
															int lh, int axis) {
	const survive_long_timecode *data_timecode = self->timecode[sensor_idx][lh];
	if (self->lh_gen != 1 && lh < 2 && self->lengths[sensor_idx][lh][axis] == 0)
		return UINT64_MAX;

	if (isnan(self->angles[sensor_idx][lh][axis]))
		return UINT64_MAX;

	return data_timecode[axis];
}

survive_long_timecode SurviveSensorActivations_time_since_last_reading(const SurviveSensorActivations *self,
																  uint32_t sensor_idx, int lh, int axis) {
	survive_long_timecode last_reading = SurviveSensorActivations_last_reading(self, sensor_idx, lh, axis);
	survive_long_timecode timecode_now = self->last_light;

	if (last_reading > timecode_now)
		return UINT32_MAX;

	return timecode_now - last_reading;
}

bool SurviveSensorActivations_isPairValid(const SurviveSensorActivations *self, uint32_t tolerance,
										  uint32_t timecode_now, uint32_t idx, int lh) {
	const survive_long_timecode *data_timecode = self->timecode[idx][lh];
	if (self->lh_gen != 1 && (self->lengths[idx][lh][0] == 0 || self->lengths[idx][lh][1] == 0))
		return false;

	if (isnan(self->angles[idx][lh][0]) || isnan(self->angles[idx][lh][1]))
		return false;

	return !(timecode_now - data_timecode[0] > tolerance || timecode_now - data_timecode[1] > tolerance);
}

survive_long_timecode SurviveSensorActivations_last_time(const SurviveSensorActivations *self) {
	survive_long_timecode last_time = self->last_light;
	if (self->last_imu > last_time) {
		last_time = self->last_imu;
	}
	return last_time;
}
survive_long_timecode SurviveSensorActivations_stationary_time(const SurviveSensorActivations *self) {
	survive_long_timecode last_time = SurviveSensorActivations_last_time(self);
	survive_long_timecode last_move = self->last_movement;
	if (last_move == 0)
		return 0;

	assert(last_move <= last_time);
	return last_time - last_move;
}

void SurviveSensorActivations_register_runtime(SurviveSensorActivations *self, survive_long_timecode tc,
											   uint64_t runtime_clock) {
	double runtime_offset = runtime_clock - (uint64_t)(tc * 0.0208333333);
	if (self->runtime_offset == 0)
		self->runtime_offset = runtime_offset;
	else {
		self->runtime_offset = self->runtime_offset * .90 + .1 * runtime_offset;
	}
}

uint64_t SurviveSensorActivations_runtime(SurviveSensorActivations *self, survive_long_timecode tc) {
	return self->runtime_offset + (uint64_t)(tc * 0.0208333333);
}

void SurviveSensorActivations_add_imu(SurviveSensorActivations *self, struct PoserDataIMU *imuData) {
	self->last_imu = imuData->hdr.timecode;
	// fprintf(stderr, "imu tc: %f\n", self->last_imu/ 48000000.);
	if (self->imu_init_cnt > 0) {
		self->imu_init_cnt--;
		return;
	}

	if (isnan(self->accel[0])) {
		for (int i = 0; i < 3; i++) {
			self->accel[i] = imuData->accel[i];
			self->gyro[i] = imuData->gyro[i];
			self->mag[i] = imuData->mag[i];
		}
		self->last_movement = imuData->hdr.timecode;
	} else {
		for (int i = 0; i < 3; i++) {
			self->accel[i] = .98 * self->accel[i] + .02 * imuData->accel[i];
			self->gyro[i] = .98 * self->gyro[i] + .02 * imuData->gyro[i];
			self->mag[i] = .98 * self->mag[i] + .02 * imuData->mag[i];
		}
	}
	struct SurviveObject *so = self->so;
	SV_DATA_LOG("accel running average", self->accel, 3);

	if (norm3d(imuData->gyro) > moveThresholdGyro || dist3d(self->accel, imuData->accel) > moveThresholdAcc) {
		self->last_movement = imuData->hdr.timecode;
		// fprintf(stderr, "%f %f\n", norm3d(imuData->gyro), dist3d(self->accel, imuData->accel));
	}
}

static inline FLT norm_pdf(FLT x, FLT mean, FLT std) {
	const FLT scale = 1. / sqrt(M_PI * 2);
	FLT ratio = (x - mean) / std;
	ratio = (ratio * ratio) * -.5;
	return scale * exp(ratio);
}

static inline void SurviveSensorActivations_update_center(SurviveSensorActivations *self, FLT alpha, int lh, int axis,
														  FLT oldval, FLT angle) {
	FLT *mean_sum = &self->angles_center_x[lh][axis];
	FLT *dev = &self->angles_center_dev[lh][axis];
	int *cnt = &self->angles_center_cnt[lh][axis];

	if (*cnt) {
		FLT beta = 1. - alpha;
		*mean_sum *= beta;
		*dev *= beta;
		if (!isfinite(oldval))
			(*cnt)++;

		FLT var = (*mean_sum - angle);
		*dev += alpha * var * var;
		*mean_sum += alpha * angle;
	} else {
		(*cnt)++;
		*mean_sum = angle;
		*dev = 0;
	}
}
static inline bool SurviveSensorActivations_check_outlier(SurviveSensorActivations *self, int sensor_id, int lh,
														  int axis, survive_long_timecode timecode, FLT angle) {
	FLT *oldangle = &self->angles[sensor_id][lh][axis];
	if (self->angles_center_dev[lh][axis] == 0) {
		goto accept_data;
	}

	const survive_long_timecode *data_timecode = &self->timecode[sensor_id][lh][axis];
	FLT change_rate = fabs(*oldangle - angle) / (FLT)(timecode - *data_timecode) * 48000000.;
	if (*data_timecode != 0 && change_rate > filterLightChange) {
		goto reject_data;
	}

	FLT dev = self->angles_center_dev[lh][axis];
	if (dev < .1)
		dev = .1;

	FLT P = norm_pdf(angle, self->angles_center_x[lh][axis], dev);
	int cnt = self->angles_center_cnt[lh][axis];
	if (self->so)
		cnt = (int)self->so->sensor_ct;

	FLT chauvenet_criterion = P * cnt;
	if (chauvenet_criterion < filterOutlierCriteria) {
		goto reject_data;
	}

accept_data:
	SurviveSensorActivations_update_center(self, .1, lh, axis, *oldangle, angle);
	return false;
reject_data:
	if (self->so && self->so->ctx) {
		SurviveContext *ctx = self->so->ctx;

		SV_VERBOSE(105, "Rejecting outlier %f(%f) for %2d.%2d.%d (P %7.7f, %7.7f)", angle, *oldangle, lh, sensor_id,
				   axis, P, chauvenet_criterion);
	}
	SurviveSensorActivations_update_center(self, .05, lh, axis, *oldangle, angle);
	return true;
}
SURVIVE_EXPORT void SurviveSensorActivations_valid_counts(SurviveSensorActivations *self,
														  survive_long_timecode tolerance, uint32_t *meas_cnt,
														  uint32_t *lh_count, uint32_t *axis_cnt,
														  size_t *meas_for_lhs_axis) {
	survive_timecode sensor_time_window = tolerance == 0 ? SurviveSensorActivations_default_tolerance : tolerance;
	SurviveContext *ctx = self->so->ctx;
	for (int lh = 0; lh < ctx->activeLighthouses; lh++) {
		if (!ctx->bsd[lh].PositionSet) {
			continue;
		}
		bool seenLH = false;
		for (uint8_t sensor = 0; sensor < self->so->sensor_ct; sensor++) {
			bool seenAxis = false;
			for (uint8_t axis = 0; axis < 2; axis++) {
				survive_timecode last_reading =
					SurviveSensorActivations_time_since_last_reading(self, sensor, lh, axis);
				bool isReadingValue = last_reading < sensor_time_window;

				if (isReadingValue) {
					if (meas_cnt)
						(*meas_cnt)++;
					if (axis_cnt && !seenAxis)
						(*axis_cnt)++;
					if (lh_count && !seenLH)
						(*lh_count)++;
					seenLH = true;
					seenAxis = true;
					if (meas_for_lhs_axis) {
						meas_for_lhs_axis[lh * 2 + axis]++;
					}
				}
			}
		}
	}
}
bool SurviveSensorActivations_add_gen2(SurviveSensorActivations *self, struct PoserDataLightGen2 *lightData) {
	self->lh_gen = 1;

	if(lightData->common.hdr.pt == POSERDATA_LIGHT_GEN2) {
		int axis = lightData->plane;
		PoserDataLight *l = &lightData->common;
		if (l->sensor_id >= SENSORS_PER_OBJECT)
			return false;

		survive_long_timecode *data_timecode = &self->timecode[l->sensor_id][l->lh][axis];
		FLT *angle = &self->angles[l->sensor_id][l->lh][axis];

		if (!SurviveSensorActivations_check_outlier(self, l->sensor_id, l->lh, axis, l->hdr.timecode, l->angle)) {
			survive_long_timecode long_timecode = l->hdr.timecode;

			if (!isnan(*angle) && fabs(*angle - l->angle) > moveThresholdAng) {
				self->last_light_change = self->last_movement = long_timecode;
			}

			if (isnan(*angle))
				self->last_light_change = long_timecode;

			// fprintf(stderr, "Time %f\n", l->hdr.timecode / 48000000.);
			*data_timecode = l->hdr.timecode;
			*angle = l->angle;
		} else {
			return false;
		}
	}

	if(lightData->common.hdr.timecode > self->last_light) {
		self->last_light = lightData->common.hdr.timecode;
	}
	return true;
}

SURVIVE_EXPORT void SurviveSensorActivations_reset(SurviveSensorActivations *self) {
	struct SurviveObject *so = self->so;
	memset(self, 0, sizeof(SurviveSensorActivations));
	self->so = so;

	for (int i = 0; i < SENSORS_PER_OBJECT; i++) {
		for (int j = 0; j < NUM_GEN2_LIGHTHOUSES; j++) {
			for (int h = 0; h < 2; h++) {
				self->angles[i][j][h] = NAN;
				self->angles_center_x[j][h] = NAN;
			}
		}
	}

	for (int i = 0; i < 3; i++) {
		self->accel[i] = NAN;
	}

	self->imu_init_cnt = 30;
}
SURVIVE_EXPORT void SurviveSensorActivations_ctor(SurviveObject *so, SurviveSensorActivations *self) {
	if (so) {
		moveThresholdAcc = survive_configf(so->ctx, MOVMENT_THRESHOLD_ACC_TAG, SC_GET, 0);
		moveThresholdGyro = survive_configf(so->ctx, MOVMENT_THRESHOLD_GYRO_TAG, SC_GET, 0);
		moveThresholdAng = survive_configf(so->ctx, MOVMENT_THRESHOLD_ANG_TAG, SC_GET, 0);
		filterLightChange = survive_configf(so->ctx, FILTER_THRESHOLD_ANG_TAG, SC_GET, 0);
		filterOutlierCriteria = survive_configf(so->ctx, FILTER_LIGHT_OUTLIER_CRITERIA_TAG, SC_GET, 0.5);
	}

	SurviveSensorActivations_reset(self);
	self->so = so;
	self->lh_gen = -1;
}

bool SurviveSensorActivations_add(SurviveSensorActivations *self, struct PoserDataLightGen1 *_lightData) {
	self->lh_gen = 0;

	int axis = (_lightData->acode & 1);
	PoserDataLight *lightData = &_lightData->common;
	survive_long_timecode *data_timecode = &self->timecode[lightData->sensor_id][lightData->lh][axis];

	FLT *angle = &self->angles[lightData->sensor_id][lightData->lh][axis];

	if (SurviveSensorActivations_check_outlier(self, lightData->sensor_id, lightData->lh, axis, lightData->hdr.timecode,
											   lightData->angle)) {
		return false;
	}

	uint32_t *length = &self->lengths[lightData->sensor_id][lightData->lh][axis];

	self->hits[lightData->sensor_id][lightData->lh][axis]++;
	if (*length == 0 || fabs(*angle - lightData->angle) > moveThresholdAng) {
		survive_long_timecode long_timecode = lightData->hdr.timecode;
		// assert(long_timecode > self->last_movement);
		self->last_light_change = self->last_movement = long_timecode;
	}

	SurviveContext *ctx = self->so->ctx;

	*angle = lightData->angle;
	*data_timecode = lightData->hdr.timecode;
	*length = (uint32_t)(_lightData->length * 48000000);
	if (lightData->hdr.timecode > self->last_light) {
		if (self->last_light != 0 && lightData->hdr.timecode - self->last_light > 480000000) {
			SV_ERROR(4, "Bad update");
		}
		// SV_WARN("Updating last_light %lx", lightData->hdr.timecode);
		self->last_light = lightData->hdr.timecode;
	}

	static int bad_time_cnt = 0;
	if (self->last_imu != 0 && fabs(lightData->hdr.timecode / 48000000. - self->last_imu / 48000000.) > 1) {
		bad_time_cnt++;
		SV_WARN("%s Bad time %f vs %f", survive_colorize(self->so->codename), lightData->hdr.timecode / 48000000.,
				self->last_imu / 48000000.);
		if (bad_time_cnt > 10) {
			SV_ERROR(4, "Too many bad_time events");
		}
	}
	return true;
	// fprintf(stderr, "lightcap tc: %f\n", lightData->hdr.timecode/ 48000000.);
}

static inline survive_long_timecode make_long_timecode(survive_long_timecode prev, survive_timecode current) {
	survive_long_timecode rtn = current | (prev & 0xFFFFFFFF00000000);

	if(rtn < prev && rtn + 0x80000000 < prev) {
		rtn += 0x100000000;
	}
	if (rtn > prev && prev + 0x80000000 < rtn && rtn > 0x100000000) {
		rtn -= 0x100000000;
	}
	return rtn;
}
SURVIVE_EXPORT survive_long_timecode SurviveSensorActivations_long_timecode_imu(const SurviveSensorActivations *self, survive_timecode timecode) {
	return make_long_timecode(self->last_imu, timecode);
}
SURVIVE_EXPORT survive_long_timecode SurviveSensorActivations_long_timecode_light(const SurviveSensorActivations *self, survive_timecode timecode) {
	return make_long_timecode(self->last_light, timecode);
}


FLT SurviveSensorActivations_difference(const SurviveSensorActivations *rhs, const SurviveSensorActivations *lhs) {
	FLT rtn = 0;
	int cnt = 0;
	for(size_t i = 0;i < SENSORS_PER_OBJECT;i++) {
		for (size_t lh = 0; lh < NUM_GEN1_LIGHTHOUSES; lh++) {
			for(size_t axis = 0;axis < 2;axis++) {
				if(rhs->lengths[i][lh][axis] > 0 && lhs->lengths[i][lh][axis] > 0) {
					FLT diff = rhs->angles[i][lh][axis] - lhs->angles[i][lh][axis];
					rtn += diff * diff;
					cnt++;
				}
			}
		}
	}
	return rtn / (FLT)cnt;
}

SURVIVE_EXPORT uint32_t SurviveSensorActivations_default_tolerance =
	(uint32_t)(48000000 /*mhz*/ * (16.7 /*ms*/) / 1000) + 5000;
