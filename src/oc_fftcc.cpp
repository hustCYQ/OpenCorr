/*
 * This file is part of OpenCorr, an open source C++ library for
 * study and development of 2D, 3D/stereo and volumetric
 * digital image correlation.
 *
 * Copyright (C) 2021, Zhenyu Jiang <zhenyujiang@scut.edu.cn>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one from http://mozilla.org/MPL/2.0/.
 *
 * More information about OpenCorr can be found at https://www.opencorr.org/
 */

#include <math.h>
#include <omp.h>
#include "oc_fftcc.h"

namespace opencorr {

	FFTW* FFTW::allocate(int subset_radius_x, int subset_radius_y) {
		int width = 2 * subset_radius_x;
		int height = 2 * subset_radius_y;
		int buffer_length = width * (subset_radius_y + 1);
		unsigned int subset_size = width * height;

		FFTW* FFTW_instance = new FFTW;

		FFTW_instance->ref_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);
		FFTW_instance->tar_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);
		FFTW_instance->zncc_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);

		FFTW_instance->ref_subset = new float[subset_size];
		FFTW_instance->tar_subset = new float[subset_size];
		FFTW_instance->zncc = new float[subset_size];

#pragma omp critical 
		{
			FFTW_instance->ref_plan = fftwf_plan_dft_r2c_2d(width, height, FFTW_instance->ref_subset, FFTW_instance->ref_freq, FFTW_ESTIMATE);
			FFTW_instance->tar_plan = fftwf_plan_dft_r2c_2d(width, height, FFTW_instance->tar_subset, FFTW_instance->tar_freq, FFTW_ESTIMATE);
			FFTW_instance->zncc_plan = fftwf_plan_dft_c2r_2d(width, height, FFTW_instance->zncc_freq, FFTW_instance->zncc, FFTW_ESTIMATE);
		}
		return FFTW_instance;
	}

	FFTW* FFTW::allocate(int subset_radius_x, int subset_radius_y, int subset_radius_z) {
		int dim_x = 2 * subset_radius_x;
		int dim_y = 2 * subset_radius_y;
		int dim_z = 2 * subset_radius_z;
		int buffer_length = dim_x * dim_y * (subset_radius_z + 1);
		unsigned int subset_size = dim_x * dim_y * dim_z;

		FFTW* FFTW_instance = new FFTW;

		FFTW_instance->ref_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);
		FFTW_instance->tar_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);
		FFTW_instance->zncc_freq = (fftwf_complex*)fftw_malloc(sizeof(fftwf_complex) * buffer_length);

		FFTW_instance->ref_subset = new float[subset_size];
		FFTW_instance->tar_subset = new float[subset_size];
		FFTW_instance->zncc = new float[subset_size];

#pragma omp critical 
		{
			FFTW_instance->ref_plan = fftwf_plan_dft_r2c_3d(dim_x, dim_y, dim_z, FFTW_instance->ref_subset, FFTW_instance->ref_freq, FFTW_ESTIMATE);
			FFTW_instance->tar_plan = fftwf_plan_dft_r2c_3d(dim_x, dim_y, dim_z, FFTW_instance->tar_subset, FFTW_instance->tar_freq, FFTW_ESTIMATE);
			FFTW_instance->zncc_plan = fftwf_plan_dft_c2r_3d(dim_x, dim_y, dim_z, FFTW_instance->zncc_freq, FFTW_instance->zncc, FFTW_ESTIMATE);
		}
		return FFTW_instance;
	}

	void FFTW::release(FFTW* instance) {
		delete[] instance->ref_subset;
		delete[] instance->tar_subset;
		delete[] instance->zncc;
		fftw_free(instance->ref_freq);
		fftw_free(instance->tar_freq);
		fftw_free(instance->zncc_freq);
		fftwf_destroy_plan(instance->ref_plan);
		fftwf_destroy_plan(instance->tar_plan);
		fftwf_destroy_plan(instance->zncc_plan);
	}

	//FFT accelerated cross correlation 2D
	FFTCC2D::FFTCC2D(int subset_radius_x, int subset_radius_y, int thread_number) {
		this->subset_radius_x = subset_radius_x;
		this->subset_radius_y = subset_radius_y;
		this->thread_number = thread_number;

		for (int i = 0; i < thread_number; i++) {
			FFTW* instance = FFTW::allocate(subset_radius_x, subset_radius_y);
			instance_pool.push_back(instance);
		}
	}

	FFTCC2D::~FFTCC2D() {
		for (auto& instance : instance_pool) {
			FFTW::release(instance);
			delete instance;
		}
		instance_pool.clear();
	}

	FFTW* FFTCC2D::getInstance(int tid) {
		if (tid >= (int)instance_pool.size()) {
			throw std::string("CPU thread ID over limit");
		}

		return instance_pool[tid];
	}

	void FFTCC2D::compute(POI2D* poi) {
		//set instance w.r.t. thread id 
		FFTW* current_instance = getInstance(omp_get_thread_num());

		int subset_width = subset_radius_x * 2;
		int subset_height = subset_radius_y * 2;
		int subset_size = subset_width * subset_height;

		//set initial guess of displacement as offset
		Point2D initial_displacement(poi->deformation.u, poi->deformation.v);

		//initialize mean and norm in subsets
		float ref_mean = 0;
		float tar_mean = 0;
		float ref_norm = 0;
		float tar_norm = 0;

		for (int r = 0; r < subset_height; r++) {
			for (int c = 0; c < subset_width; c++) {
				//fill reference subset
				Point2D ref_point(poi->x + c - subset_radius_x, poi->y + r - subset_radius_y);
				float value = ref_img->eg_mat((int)ref_point.y, (int)ref_point.x);
				current_instance->ref_subset[r * subset_width + c] = value;
				ref_mean += value;

				//fill the target subset with initial guess of displacement
				Point2D tar_point = ref_point + initial_displacement;
				value = tar_img->eg_mat((int)tar_point.y, (int)tar_point.x);
				current_instance->tar_subset[r * subset_width + c] = value;
				tar_mean += value;
			}
		}
		ref_mean /= subset_size;
		tar_mean /= subset_size;

		for (int i = 0; i < subset_size; i++) {
			current_instance->ref_subset[i] -= ref_mean;
			current_instance->tar_subset[i] -= tar_mean;
			ref_norm += current_instance->ref_subset[i] * current_instance->ref_subset[i];
			tar_norm += current_instance->tar_subset[i] * current_instance->tar_subset[i];
		}

		fftwf_execute(current_instance->ref_plan);
		fftwf_execute(current_instance->tar_plan);

		int buffer_length = subset_width * (subset_radius_y + 1);
		for (int n = 0; n < buffer_length; n++) {
			current_instance->zncc_freq[n][0] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][0])
				+ (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][1]);
			current_instance->zncc_freq[n][1] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][1])
				- (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][0]);
		}

		fftwf_execute(current_instance->zncc_plan);

		//search for max ZCC, then normalize it to get ZNCC
		float max_zncc = -2;
		int max_zncc_index = 0;
		for (int i = 0; i < subset_size; i++)
		{
			if (current_instance->zncc[i] > max_zncc)
			{
				max_zncc = current_instance->zncc[i];
				max_zncc_index = i;
			}
		}
		int local_displacement_u = max_zncc_index % subset_width;
		int local_displacement_v = max_zncc_index / subset_width;

		if (local_displacement_u > subset_radius_x)
			local_displacement_u -= subset_width;
		if (local_displacement_v > subset_radius_y)
			local_displacement_v -= subset_height;

		//store the final results
		poi->deformation.u = (float)local_displacement_u + initial_displacement.x;
		poi->deformation.v = (float)local_displacement_v + initial_displacement.y;

		poi->result.u0 = initial_displacement.x;
		poi->result.v0 = initial_displacement.y;
		poi->result.zncc = max_zncc / (sqrt(ref_norm * tar_norm) * subset_size);
	}

	void FFTCC2D::compute(std::vector<POI2D>& poi_queue) {
#pragma omp parallel for
		for (int i = 0; i < poi_queue.size(); ++i) {
			compute(&poi_queue[i]);
		}
	}

	Point2D FFTCC2D::determineSpeckleSize(POI2D* poi, float half_peak_ratio) {
		//set instance w.r.t. thread id 
		FFTW* current_instance = getInstance(omp_get_thread_num());

		int subset_width = subset_radius_x * 2;
		int subset_height = subset_radius_y * 2;
		int subset_size = subset_width * subset_height;

		//initialize mean and norm in subsets
		float ref_mean = 0;
		float ref_norm = 0;

		for (int r = 0; r < subset_height; r++) {
			for (int c = 0; c < subset_width; c++) {
				Point2D ref_point(poi->x + c - subset_radius_x, poi->y + r - subset_radius_y);
				float value = ref_img->eg_mat((int)ref_point.y, (int)ref_point.x);
				current_instance->ref_subset[r * subset_width + c] = value;
				ref_mean += value;
			}
		}
		ref_mean /= subset_size;

		//set zero-mean grayscale values in reference subset and target subset
		for (int i = 0; i < subset_size; i++) {
			current_instance->ref_subset[i] -= ref_mean;
			current_instance->tar_subset[i] = current_instance->ref_subset[i];
			ref_norm += current_instance->ref_subset[i] * current_instance->ref_subset[i];
		}

		//calculate autocorrelation
		fftwf_execute(current_instance->ref_plan);
		fftwf_execute(current_instance->tar_plan);

		int buffer_length = subset_width * (subset_radius_y + 1);
		for (int n = 0; n < buffer_length; n++) {
			current_instance->zncc_freq[n][0] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][0])
				+ (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][1]);
			current_instance->zncc_freq[n][1] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][1])
				- (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][0]);
		}

		fftwf_execute(current_instance->zncc_plan);

		//normalize the zncc and shift the peak to the center of subset
		Eigen::MatrixXf zncc_matrix = Eigen::MatrixXf::Zero(subset_height, subset_width);
		ref_norm *= subset_size;
		for (int i = 0; i < subset_size; i++) {
			current_instance->zncc[i] /= ref_norm;
			int shift_c = i % subset_width;
			int shift_r = i / subset_width;
			if (shift_c > subset_radius_x)
				shift_c -= subset_width;
			if (shift_r > subset_radius_y)
				shift_r -= subset_height;
			shift_c += subset_radius_x - 1;
			shift_r += subset_radius_y - 1;

			zncc_matrix(shift_r, shift_c) = current_instance->zncc[i];
		}

		//determine speckle size along axies x and y
		float rx1 = 0;
		float rx2 = 0;
		float ry1 = 0;
		float ry2 = 0;
		int y0 = subset_radius_y - 1;
		int x0 = subset_radius_x - 1;

		//search half-peak breadth along x
		for (int i = 0; i < x0; i++) {
			int x1 = x0 + i;
			int x2 = x1 + 1;
			if (zncc_matrix(y0, x1) > half_peak_ratio && zncc_matrix(y0, x2) <= half_peak_ratio) {
				rx1 = x2 - (x2 - x1) * (half_peak_ratio - zncc_matrix(y0, x2)) * (zncc_matrix(y0, x1) - zncc_matrix(y0, x2));
				break;
			}
		}
		for (int i = 0; i < x0; i++) {
			int x1 = x0 - i;
			int x2 = x1 - 1;
			if (zncc_matrix(y0, x1) > half_peak_ratio && zncc_matrix(y0, x2) <= half_peak_ratio) {
				rx2 = x2 - (x2 - x1) * (half_peak_ratio - zncc_matrix(y0, x2)) * (zncc_matrix(y0, x1) - zncc_matrix(y0, x2));
				break;
			}
		}
		//search half-peak breadth along y
		for (int i = 0; i < y0; i++) {
			int y1 = y0 + i;
			int y2 = y1 + 1;
			if (zncc_matrix(y1, x0) > half_peak_ratio && zncc_matrix(y2, x0) <= half_peak_ratio) {
				ry1 = y2 - (y2 - y1) * (half_peak_ratio - zncc_matrix(y2, x0)) * (zncc_matrix(y1, x0) - zncc_matrix(y2, x0));
				break;
			}
		}
		for (int i = 0; i < y0; i++) {
			int y1 = y0 - i;
			int y2 = y1 - 1;
			if (zncc_matrix(y1, x0) > half_peak_ratio && zncc_matrix(y2, x0) <= half_peak_ratio) {
				ry2 = y2 - (y2 - y1) * (half_peak_ratio - zncc_matrix(y2, x0)) * (zncc_matrix(y1, x0) - zncc_matrix(y2, x0));
				break;
			}
		}

		Point2D speckle_size((rx1 - rx2), (ry1 - ry2));
		return speckle_size;
	}



	//FFT accelerated cross correlation 3D
	FFTCC3D::FFTCC3D(int subset_radius_x, int subset_radius_y, int subset_radius_z, int thread_number) {
		this->subset_radius_x = subset_radius_x;
		this->subset_radius_y = subset_radius_y;
		this->subset_radius_z = subset_radius_z;
		this->thread_number = thread_number;

		for (int i = 0; i < thread_number; i++) {
			FFTW* instance = FFTW::allocate(subset_radius_x, subset_radius_y, subset_radius_z);
			instance_pool.push_back(instance);
		}
	}

	FFTCC3D::~FFTCC3D() {
		for (auto& instance : instance_pool) {
			FFTW::release(instance);
			delete instance;
		}
		instance_pool.clear();
	}

	FFTW* FFTCC3D::getInstance(int tid) {
		if (tid >= (int)instance_pool.size()) {
			throw std::string("CPU thread ID over limit");
		}

		return instance_pool[tid];
	}

	void FFTCC3D::compute(POI3D* poi) {
		//set instance w.r.t. thread id 
		FFTW* current_instance = getInstance(omp_get_thread_num());

		int subset_dim_x = subset_radius_x * 2;
		int subset_dim_y = subset_radius_y * 2;
		int subset_dim_z = subset_radius_z * 2;
		int subset_size = subset_dim_x * subset_dim_y * subset_dim_z;

		//set initial guess of displacement as offset
		Point3D initial_displacement(poi->deformation.u, poi->deformation.v, poi->deformation.w);

		//initialize mean and norm in subsets
		float ref_mean = 0;
		float tar_mean = 0;
		float ref_norm = 0;
		float tar_norm = 0;

		for (int i = 0; i < subset_dim_z; i++) {
			for (int j = 0; j < subset_dim_y; j++) {
				for (int k = 0; k < subset_dim_x; k++) {
					//fill the reference subset
					Point3D ref_point(poi->x + k - subset_radius_x, poi->y + j - subset_radius_y, poi->z + i - subset_radius_z);
					float value = ref_img->vol_mat[(int)ref_point.z][(int)ref_point.y][(int)ref_point.x];
					current_instance->ref_subset[(i * subset_dim_y + j) * subset_dim_x + k] = value;
					ref_mean += value;

					//fill the target subset with initial guess of displacement
					Point3D tar_point = ref_point + initial_displacement;
					value = tar_img->vol_mat[(int)tar_point.z][(int)tar_point.y][(int)tar_point.x];
					current_instance->tar_subset[(i * subset_dim_y + j) * subset_dim_x + k] = value;
					tar_mean += value;
				}
			}
		}
		ref_mean /= subset_size;
		tar_mean /= subset_size;

		for (int i = 0; i < subset_size; i++) {
			current_instance->ref_subset[i] -= ref_mean;
			current_instance->tar_subset[i] -= tar_mean;
			ref_norm += current_instance->ref_subset[i] * current_instance->ref_subset[i];
			tar_norm += current_instance->tar_subset[i] * current_instance->tar_subset[i];
		}

		fftwf_execute(current_instance->ref_plan);
		fftwf_execute(current_instance->tar_plan);

		unsigned int buffer_length = subset_dim_x * subset_dim_y * (subset_radius_z + 1);
		for (unsigned int n = 0; n < buffer_length; n++) {
			current_instance->zncc_freq[n][0] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][0])
				+ (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][1]);
			current_instance->zncc_freq[n][1] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][1])
				- (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][0]);
		}

		fftwf_execute(current_instance->zncc_plan);

		//search for max ZCC, then normalize it to get ZNCC
		float max_zncc = -2;
		int max_zncc_index = 0;
		for (int i = 0; i < subset_size; i++)
		{
			if (current_instance->zncc[i] > max_zncc)
			{
				max_zncc = current_instance->zncc[i];
				max_zncc_index = i;
			}
		}
		int local_displacement_u = max_zncc_index % subset_dim_x;
		int local_displacement_v = (max_zncc_index / subset_dim_x) % subset_dim_y;
		int local_displacement_w = max_zncc_index / (subset_dim_x * subset_dim_y);

		if (local_displacement_u > subset_radius_x)
			local_displacement_u -= subset_dim_x;
		if (local_displacement_v > subset_radius_y)
			local_displacement_v -= subset_dim_y;
		if (local_displacement_w > subset_radius_z)
			local_displacement_w -= subset_dim_z;

		//store the final results
		poi->deformation.u = (float)local_displacement_u + initial_displacement.x;
		poi->deformation.v = (float)local_displacement_v + initial_displacement.y;
		poi->deformation.w = (float)local_displacement_w + initial_displacement.z;

		poi->result.u0 = initial_displacement.x;
		poi->result.v0 = initial_displacement.y;
		poi->result.w0 = initial_displacement.z;
		poi->result.zncc = max_zncc / (sqrt(ref_norm * tar_norm) * subset_size);
	}

	void FFTCC3D::compute(std::vector<POI3D>& poi_queue) {
#pragma omp parallel for
		for (int i = 0; i < poi_queue.size(); ++i) {
			compute(&poi_queue[i]);
		}
	}

	Point3D FFTCC3D::determineSpeckleSize(POI3D* poi, float half_peak_ratio) {
		//set instance w.r.t. thread id 
		FFTW* current_instance = getInstance(omp_get_thread_num());

		int subset_dim_x = subset_radius_x * 2;
		int subset_dim_y = subset_radius_y * 2;
		int subset_dim_z = subset_radius_z * 2;
		int subset_size = subset_dim_x * subset_dim_y * subset_dim_z;

		//initialize mean and norm in subsets
		float ref_mean = 0;
		float ref_norm = 0;

		for (int i = 0; i < subset_dim_z; i++) {
			for (int j = 0; j < subset_dim_y; j++) {
				for (int k = 0; k < subset_dim_x; k++) {
					Point3D ref_point(poi->x + k - subset_radius_x, poi->y + j - subset_radius_y, poi->z + i - subset_radius_z);
					float value = ref_img->vol_mat[(int)ref_point.z][(int)ref_point.y][(int)ref_point.x];
					current_instance->ref_subset[(i * subset_dim_y + j) * subset_dim_x + k] = value;
					ref_mean += value;
				}
			}
		}
		ref_mean /= subset_size;

		for (int i = 0; i < subset_size; i++) {
			current_instance->ref_subset[i] -= ref_mean;
			ref_norm += current_instance->ref_subset[i] * current_instance->ref_subset[i];
			current_instance->tar_subset[i] = current_instance->ref_subset[i];
		}

		fftwf_execute(current_instance->ref_plan);
		fftwf_execute(current_instance->tar_plan);

		unsigned int buffer_length = subset_dim_x * subset_dim_y * (subset_radius_z + 1);
		for (unsigned int n = 0; n < buffer_length; n++) {
			current_instance->zncc_freq[n][0] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][0])
				+ (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][1]);
			current_instance->zncc_freq[n][1] = (current_instance->ref_freq[n][0] * current_instance->tar_freq[n][1])
				- (current_instance->ref_freq[n][1] * current_instance->tar_freq[n][0]);
		}

		fftwf_execute(current_instance->zncc_plan);

		//normalize the zncc and shift the peak to the center of subset
		float*** zncc_matrix = new3D(subset_dim_z, subset_dim_y, subset_dim_x);
		ref_norm *= subset_size;
		for (int i = 0; i < subset_size; i++) {
			current_instance->zncc[i] /= ref_norm;

			int shift_x = i % subset_dim_x;
			int shift_y = (i / subset_dim_x) % subset_dim_y;
			int shift_z = i / (subset_dim_x * subset_dim_y);
			if (shift_x > subset_radius_x)
				shift_x -= subset_dim_x;
			if (shift_y > subset_radius_y)
				shift_y -= subset_dim_y;
			if (shift_z > subset_radius_z)
				shift_z -= subset_dim_z;
			shift_x += subset_radius_x - 1;
			shift_y += subset_radius_y - 1;
			shift_z += subset_radius_z - 1;

			zncc_matrix[shift_z][shift_y][shift_x] = current_instance->zncc[i];
		}

		//determine speckle size along axies x and y
		float rx1 = 0;
		float rx2 = 0;
		float ry1 = 0;
		float ry2 = 0;
		float rz1 = 0;
		float rz2 = 0;
		int z0 = subset_radius_z - 1;
		int y0 = subset_radius_y - 1;
		int x0 = subset_radius_x - 1;

		//search half-peak breadth along x
		for (int i = 0; i < x0; i++) {
			int x1 = x0 + i;
			int x2 = x1 + 1;
			if (zncc_matrix[z0][y0][x1] > half_peak_ratio && zncc_matrix[z0][y0][x2] <= half_peak_ratio) {
				rx1 = x2 - (x2 - x1) * (half_peak_ratio - zncc_matrix[z0][y0][x2]) * (zncc_matrix[z0][y0][x1] - zncc_matrix[z0][y0][x2]);
				break;
			}
		}
		for (int i = 0; i < x0; i++) {
			int x1 = x0 - i;
			int x2 = x1 - 1;
			if (zncc_matrix[z0][y0][x1] > half_peak_ratio && zncc_matrix[z0][y0][x2] <= half_peak_ratio) {
				rx2 = x2 - (x2 - x1) * (half_peak_ratio - zncc_matrix[z0][y0][x2]) * (zncc_matrix[z0][y0][x1] - zncc_matrix[z0][y0][x2]);
				break;
			}
		}

		//search half-peak breadth along y
		for (int i = 0; i < y0; i++) {
			int y1 = y0 + i;
			int y2 = y1 + 1;
			if (zncc_matrix[z0][y1][x0] > half_peak_ratio && zncc_matrix[z0][y2][x0] <= half_peak_ratio) {
				ry1 = y2 - (y2 - y1) * (half_peak_ratio - zncc_matrix[z0][y2][x0]) * (zncc_matrix[z0][y1][x0] - zncc_matrix[z0][y2][x0]);
				break;
			}
		}
		for (int i = 0; i < y0 - 1; i++) {
			int y1 = y0 - i;
			int y2 = y1 - 1;
			if (zncc_matrix[z0][y1][x0] > half_peak_ratio && zncc_matrix[z0][y2][x0] <= half_peak_ratio) {
				ry2 = y2 - (y2 - y1) * (half_peak_ratio - zncc_matrix[z0][y2][x0]) * (zncc_matrix[z0][y1][x0] - zncc_matrix[z0][y2][x0]);
				break;
			}
		}

		//search half-peak breadth along z
		for (int i = 0; i < z0 - 1; i++) {
			int z1 = z0 + i;
			int z2 = z1 + 1;
			if (zncc_matrix[z1][y0][x0] > half_peak_ratio && zncc_matrix[z2][y0][x0] <= half_peak_ratio) {
				rz1 = z2 - (z2 - z1) * (half_peak_ratio - zncc_matrix[z2][y0][x0]) * (zncc_matrix[z1][y0][x0] - zncc_matrix[z2][y0][x0]);
				break;
			}
		}
		for (int i = 0; i < z0 - 1; i++) {
			int z1 = z0 - i;
			int z2 = z1 - 1;
			if (zncc_matrix[z1][y0][x0] > half_peak_ratio && zncc_matrix[z2][y0][x0] <= half_peak_ratio) {
				rz2 = z2 - (z2 - z1) * (half_peak_ratio - zncc_matrix[z2][y0][x0]) * (zncc_matrix[z1][y0][x0] - zncc_matrix[z2][y0][x0]);
				break;
			}
		}

		Point3D speckle_size((rx1 - rx2), (ry1 - ry2), (rz1 - rz2));
		delete3D(zncc_matrix);
		return speckle_size;
	}

}//namespace opencorr