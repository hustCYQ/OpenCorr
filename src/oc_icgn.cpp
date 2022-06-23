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

#include "oc_icgn.h"

namespace opencorr
{
	//DIC, the 1st order shape function
	ICGN2D1_* ICGN2D1_::allocate(int subset_radius_x, int subset_radius_y) {
		int subset_width = 2 * subset_radius_x + 1;
		int subset_height = 2 * subset_radius_y + 1;
		Point2D subset_center(0, 0);

		ICGN2D1_* ICGN_instance = new ICGN2D1_;
		ICGN_instance->ref_subset = new Subset2D(subset_center, subset_radius_x, subset_radius_y);
		ICGN_instance->tar_subset = new Subset2D(subset_center, subset_radius_x, subset_radius_y);
		ICGN_instance->error_img = Eigen::MatrixXf::Zero(subset_height, subset_width);
		ICGN_instance->sd_img = new3D(subset_height, subset_width, 6);

		return ICGN_instance;
	}

	void ICGN2D1_::release(ICGN2D1_* instance) {
		delete instance->ref_subset;
		delete instance->tar_subset;
		delete3D(instance->sd_img);
	}

	ICGN2D1_* ICGN2D1::getInstance(int tid)
	{
		if (tid >= (int)instance_pool.size()) {
			throw std::string("CPU thread ID over limit");
		}

		return instance_pool[tid];
	}

	ICGN2D1::ICGN2D1(int subset_radius_x, int subset_radius_y, float conv_criterion, float stop_condition, int thread_number)
		: ref_gradient(nullptr), tar_interp(nullptr) {
		this->subset_radius_x = subset_radius_x;
		this->subset_radius_y = subset_radius_y;
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
		this->thread_number = thread_number;

		for (int i = 0; i < thread_number; i++) {
			ICGN2D1_* instance = ICGN2D1_::allocate(subset_radius_x, subset_radius_y);
			instance_pool.push_back(instance);
		}
	}

	ICGN2D1::~ICGN2D1() {
		delete ref_gradient;
		delete tar_interp;

		for (auto& instance : instance_pool) {
			ICGN2D1_::release(instance);
			delete instance;
		}
		instance_pool.clear();
	}

	void ICGN2D1::setIteration(float conv_criterion, float stop_condition) {
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
	}

	void ICGN2D1::setIteration(POI2D* poi) {
		conv_criterion = poi->result.convergence;
		stop_condition = (int)poi->result.iteration;
	}

	void ICGN2D1::prepareRef() {
		if (ref_gradient != nullptr) {
			delete ref_gradient;
			ref_gradient = nullptr;
		}

		ref_gradient = new Gradient2D4(*ref_img);
		ref_gradient->getGradientX();
		ref_gradient->getGradientY();
	}

	void ICGN2D1::prepareTar() {
		if (tar_interp != nullptr) {
			delete tar_interp;
			tar_interp = nullptr;
		}

		tar_interp = new BicubicBspline(*tar_img);
		tar_interp->prepare();
	}

	void ICGN2D1::prepare() {
		prepareRef();
		prepareTar();
	}

	void ICGN2D1::compute(POI2D* poi) {
		//set instance w.r.t. thread id 
		ICGN2D1_* cur_instance = getInstance(omp_get_thread_num());

		if (poi->y - subset_radius_y < 0 || poi->x - subset_radius_x < 0
			|| poi->y + subset_radius_y > ref_img->height - 1 || poi->x + subset_radius_x > ref_img->width - 1
			|| std::isnan(poi->deformation.u) || std::isnan(poi->deformation.v)) {
			//std::cerr << "POI is too close to boundary" << poi->x << ", " << poi->y << std::endl;
			poi->result.zncc = -1;
		}
		else {
			int subset_width = 2 * subset_radius_x + 1;
			int subset_height = 2 * subset_radius_y + 1;

			//set reference subset
			cur_instance->ref_subset->center = (Point2D)*poi;
			cur_instance->ref_subset->fill(ref_img);
			float ref_mean_norm = cur_instance->ref_subset->zeroMeanNorm();

			//build the hessian matrix
			cur_instance->hessian.setZero();
			for (int r = 0; r < subset_height; r++) {
				for (int c = 0; c < subset_width; c++) {
					int x_local = c - subset_radius_x;
					int y_local = r - subset_radius_y;
					int x_global = (int)poi->x + x_local;
					int y_global = (int)poi->y + y_local;
					float ref_gradient_x = ref_gradient->gradient_x(y_global, x_global);
					float ref_gradient_y = ref_gradient->gradient_y(y_global, x_global);

					cur_instance->sd_img[r][c][0] = ref_gradient_x;
					cur_instance->sd_img[r][c][1] = ref_gradient_x * x_local;
					cur_instance->sd_img[r][c][2] = ref_gradient_x * y_local;
					cur_instance->sd_img[r][c][3] = ref_gradient_y;
					cur_instance->sd_img[r][c][4] = ref_gradient_y * x_local;
					cur_instance->sd_img[r][c][5] = ref_gradient_y * y_local;

					for (int i = 0; i < 6; i++) {
						for (int j = 0; j < 6; j++) {
							cur_instance->hessian(i, j) += (cur_instance->sd_img[r][c][i] * cur_instance->sd_img[r][c][j]);
						}
					}
				}
			}

			//compute inversed hessian matrix
			cur_instance->inv_hessian = cur_instance->hessian.inverse();

			//set target subset
			cur_instance->tar_subset->center = (Point2D)*poi;

			//get initial guess
			Deformation2D1 p_initial(poi->deformation.u, poi->deformation.ux, poi->deformation.uy,
				poi->deformation.v, poi->deformation.vx, poi->deformation.vy);

			//IC-GN iteration
			int iteration = 0; //initialize iteration counter
			Deformation2D1 p_current, p_increment;
			p_current.setDeformation(p_initial);
			float dp_norm_max, znssd;
			Point2D local_coor, warped_coor, global_coor;
			do {
				iteration++;
				//reconstruct target subset
				for (int r = 0; r < subset_height; r++) {
					for (int c = 0; c < subset_width; c++) {
						int x_local = c - subset_radius_x;
						int y_local = r - subset_radius_y;
						local_coor.x = x_local;
						local_coor.y = y_local;
						warped_coor = p_current.warp(local_coor);
						global_coor = cur_instance->tar_subset->center + warped_coor;
						cur_instance->tar_subset->eg_mat(r, c) = tar_interp->compute(global_coor);
					}
				}
				float tar_mean_norm = cur_instance->tar_subset->zeroMeanNorm();

				//compute error image
				cur_instance->error_img = cur_instance->tar_subset->eg_mat * (ref_mean_norm / tar_mean_norm)
					- (cur_instance->ref_subset->eg_mat);

				//calculate ZNSSD
				znssd = cur_instance->error_img.squaredNorm() / (ref_mean_norm * ref_mean_norm);

				//compute numerator
				float numerator[6] = { 0 };
				for (int r = 0; r < subset_height; r++) {
					for (int c = 0; c < subset_width; c++) {
						for (int i = 0; i < 6; i++) {
							numerator[i] += (cur_instance->sd_img[r][c][i] * cur_instance->error_img(r, c));
						}
					}
				}

				//compute dp
				float dp[6] = { 0 };
				for (int i = 0; i < 6; i++) {
					for (int j = 0; j < 6; j++) {
						dp[i] += (cur_instance->inv_hessian(i, j) * numerator[j]);
					}
				}
				p_increment.setDeformation(dp);

				//update warp
				p_current.warp_matrix = p_current.warp_matrix * p_increment.warp_matrix.inverse();

				//update p
				p_current.setDeformation();

				//check convergence
				int subset_radius_x2 = subset_radius_x * subset_radius_x;
				int subset_radius_y2 = subset_radius_y * subset_radius_y;

				dp_norm_max = 0.f;
				dp_norm_max += p_increment.u * p_increment.u;
				dp_norm_max += p_increment.ux * p_increment.ux * subset_radius_x2;
				dp_norm_max += p_increment.uy * p_increment.uy * subset_radius_y2;
				dp_norm_max += p_increment.v * p_increment.v;
				dp_norm_max += p_increment.vx * p_increment.vx * subset_radius_x2;
				dp_norm_max += p_increment.vy * p_increment.vy * subset_radius_y2;

				dp_norm_max = sqrt(dp_norm_max);
			} while (iteration < stop_condition && dp_norm_max >= conv_criterion);

			//store the final result
			poi->deformation.u = p_current.u;
			poi->deformation.ux = p_current.ux;
			poi->deformation.uy = p_current.uy;
			poi->deformation.v = p_current.v;
			poi->deformation.vx = p_current.vx;
			poi->deformation.vy = p_current.vy;

			//save the results for output
			poi->result.u0 = p_initial.u;
			poi->result.v0 = p_initial.v;
			poi->result.zncc = 0.5f * (2 - znssd);
			poi->result.iteration = (float)iteration;
			poi->result.convergence = dp_norm_max;
		}
	}

	void ICGN2D1::compute(std::vector<POI2D>& poi_queue) {
#pragma omp parallel for
		for (int i = 0; i < poi_queue.size(); ++i) {
			compute(&poi_queue[i]);
		}
	}

	//DIC, the 2nd order shape function
	ICGN2D2_* ICGN2D2_::allocate(int subset_radius_x, int subset_radius_y) {
		int subset_width = 2 * subset_radius_x + 1;
		int subset_height = 2 * subset_radius_y + 1;
		Point2D subset_center(0, 0);

		ICGN2D2_* ICGN_instance = new ICGN2D2_;
		ICGN_instance->ref_subset = new Subset2D(subset_center, subset_radius_x, subset_radius_y);
		ICGN_instance->tar_subset = new Subset2D(subset_center, subset_radius_x, subset_radius_y);
		ICGN_instance->error_img = Eigen::MatrixXf::Zero(subset_height, subset_width);
		ICGN_instance->sd_img = new3D(subset_height, subset_width, 12);

		return ICGN_instance;
	}

	void ICGN2D2_::release(ICGN2D2_* instance) {
		delete instance->ref_subset;
		delete instance->tar_subset;
		delete3D(instance->sd_img);
	}

	ICGN2D2_* ICGN2D2::getInstance(int tid)
	{
		if (tid >= (int)instance_pool.size()) {
			throw std::string("CPU thread ID over limit");
		}

		return instance_pool[tid];
	}

	ICGN2D2::ICGN2D2(int thread_number) :ref_gradient(nullptr), tar_interp(nullptr) {
		this->thread_number = thread_number;
		for (int i = 0; i < thread_number; i++) {
			ICGN2D2_* instance = ICGN2D2_::allocate(subset_radius_x, subset_radius_y);
			instance_pool.push_back(instance);
		}
	}

	ICGN2D2::ICGN2D2(int subset_radius_x, int subset_radius_y, float conv_criterion, float stop_condition, int thread_number)
		: ref_gradient(nullptr), tar_interp(nullptr) {
		this->subset_radius_x = subset_radius_x;
		this->subset_radius_y = subset_radius_y;
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
		this->thread_number = thread_number;

		for (int i = 0; i < thread_number; i++) {
			ICGN2D2_* instance = ICGN2D2_::allocate(subset_radius_x, subset_radius_y);
			instance_pool.push_back(instance);
		}
	}

	ICGN2D2::~ICGN2D2() {
		delete ref_gradient;
		delete tar_interp;

		for (auto& instance : instance_pool) {
			ICGN2D2_::release(instance);
			delete instance;
		}
		instance_pool.clear();

	}

	void ICGN2D2::setIteration(float conv_criterion, float stop_condition) {
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
	}

	void ICGN2D2::setIteration(POI2D* poi) {
		conv_criterion = poi->result.convergence;
		stop_condition = poi->result.iteration;
	}

	void ICGN2D2::prepareRef() {
		if (ref_gradient != nullptr) {
			delete ref_gradient;
			ref_gradient = nullptr;
		}

		ref_gradient = new Gradient2D4(*ref_img);
		ref_gradient->getGradientX();
		ref_gradient->getGradientY();
	}

	void ICGN2D2::prepareTar() {
		if (tar_interp != nullptr) {
			delete tar_interp;
			tar_interp = nullptr;
		}

		tar_interp = new BicubicBspline(*tar_img);
		tar_interp->prepare();
	}

	void ICGN2D2::prepare() {
		prepareRef();
		prepareTar();
	}

	void ICGN2D2::compute(POI2D* poi) {
		//set instance w.r.t. thread id 
		ICGN2D2_* cur_instance = getInstance(omp_get_thread_num());

		if (poi->y - subset_radius_y < 0 || poi->x - subset_radius_x < 0
			|| poi->y + subset_radius_y > ref_img->height - 1 || poi->x + subset_radius_x > ref_img->width - 1
			|| std::isnan(poi->deformation.u) || std::isnan(poi->deformation.v)) {
			//std::cerr << "POI is too close to boundary" << poi->x << ", " << poi->y << std::endl;
			poi->result.zncc = -1;
		}
		else {
			int subset_width = 2 * subset_radius_x + 1;
			int subset_height = 2 * subset_radius_y + 1;

			//set reference subset
			cur_instance->ref_subset->center = (Point2D)*poi;
			cur_instance->ref_subset->fill(ref_img);
			float ref_mean_norm = cur_instance->ref_subset->zeroMeanNorm();

			//build the hessian matrix
			cur_instance->hessian.setZero();
			for (int r = 0; r < subset_height; r++) {
				for (int c = 0; c < subset_width; c++) {
					int x_local = c - subset_radius_x;
					int y_local = r - subset_radius_y;
					float xx_local = (x_local * x_local) * 0.5f;
					float xy_local = (float)(x_local * y_local);
					float yy_local = (y_local * y_local) * 0.5f;
					int x_global = (int)poi->x + x_local;
					int y_global = (int)poi->y + y_local;
					float ref_gradient_x = ref_gradient->gradient_x(y_global, x_global);
					float ref_gradient_y = ref_gradient->gradient_y(y_global, x_global);

					cur_instance->sd_img[r][c][0] = ref_gradient_x;
					cur_instance->sd_img[r][c][1] = ref_gradient_x * x_local;
					cur_instance->sd_img[r][c][2] = ref_gradient_x * y_local;
					cur_instance->sd_img[r][c][3] = ref_gradient_x * xx_local;
					cur_instance->sd_img[r][c][4] = ref_gradient_x * xy_local;
					cur_instance->sd_img[r][c][5] = ref_gradient_x * yy_local;

					cur_instance->sd_img[r][c][6] = ref_gradient_y;
					cur_instance->sd_img[r][c][7] = ref_gradient_y * x_local;
					cur_instance->sd_img[r][c][8] = ref_gradient_y * y_local;
					cur_instance->sd_img[r][c][9] = ref_gradient_y * xx_local;
					cur_instance->sd_img[r][c][10] = ref_gradient_y * xy_local;
					cur_instance->sd_img[r][c][11] = ref_gradient_y * yy_local;

					for (int i = 0; i < 12; i++) {
						for (int j = 0; j < 12; j++) {
							cur_instance->hessian(i, j) += (cur_instance->sd_img[r][c][i] * cur_instance->sd_img[r][c][j]);
						}
					}
				}
			}

			//compute inversed hessian matrix
			cur_instance->inv_hessian = cur_instance->hessian.inverse();

			//set target subset
			cur_instance->tar_subset->center = (Point2D)*poi;

			//get initial guess
			Deformation2D1 p_initial(poi->deformation.u, poi->deformation.ux, poi->deformation.uy,
				poi->deformation.v, poi->deformation.vx, poi->deformation.vy);

			//IC-GN iteration
			int iteration = 0; //initialize iteration counter
			Deformation2D2 p_current, p_increment;
			p_current.setDeformation(p_initial);
			float dp_norm_max, znssd;
			Point2D local_coor, warped_coor, global_coor;
			do {
				iteration++;
				//reconstruct target subset
				for (int r = 0; r < subset_height; r++) {
					for (int c = 0; c < subset_width; c++) {
						int x_local = c - subset_radius_x;
						int y_local = r - subset_radius_y;
						local_coor.x = x_local;
						local_coor.y = y_local;
						warped_coor = p_current.warp(local_coor);
						global_coor = cur_instance->tar_subset->center + warped_coor;
						cur_instance->tar_subset->eg_mat(r, c) = tar_interp->compute(global_coor);
					}
				}
				float tar_mean_norm = cur_instance->tar_subset->zeroMeanNorm();

				//compute error image
				cur_instance->error_img = cur_instance->tar_subset->eg_mat * (ref_mean_norm / tar_mean_norm)
					- (cur_instance->ref_subset->eg_mat);

				//calculate ZNSSD
				znssd = cur_instance->error_img.squaredNorm() / (ref_mean_norm * ref_mean_norm);

				//compute numerator
				float numerator[12] = { 0 };
				for (int r = 0; r < subset_height; r++) {
					for (int c = 0; c < subset_width; c++) {
						for (int i = 0; i < 12; i++) {
							numerator[i] += (cur_instance->sd_img[r][c][i] * cur_instance->error_img(r, c));
						}
					}
				}

				//compute dp
				float dp[12] = { 0 };
				for (int i = 0; i < 12; i++) {
					for (int j = 0; j < 12; j++) {
						dp[i] += (cur_instance->inv_hessian(i, j) * numerator[j]);
					}
				}
				p_increment.setDeformation(dp);

				//update warp
				p_current.warp_matrix = p_current.warp_matrix * p_increment.warp_matrix.inverse();

				//update p
				p_current.setDeformation();

				//check convergence
				int subset_radius_x2 = subset_radius_x * subset_radius_x;
				int subset_radius_y2 = subset_radius_y * subset_radius_y;
				int subset_radius_xy = subset_radius_x2 * subset_radius_y2;

				dp_norm_max = 0.f;
				dp_norm_max += p_increment.u * p_increment.u;
				dp_norm_max += p_increment.ux * p_increment.ux * subset_radius_x2;
				dp_norm_max += p_increment.uy * p_increment.uy * subset_radius_y2;
				dp_norm_max += p_increment.uxx * p_increment.uxx * subset_radius_x2 * subset_radius_x2 / 4.f;
				dp_norm_max += p_increment.uyy * p_increment.uyy * subset_radius_y2 * subset_radius_y2 / 4.f;
				dp_norm_max += p_increment.uxy * p_increment.uxy * subset_radius_xy;

				dp_norm_max += p_increment.v * p_increment.v;
				dp_norm_max += p_increment.vx * p_increment.vx * subset_radius_x2;
				dp_norm_max += p_increment.vy * p_increment.vy * subset_radius_y2;
				dp_norm_max += p_increment.vxx * p_increment.vxx * subset_radius_x2 * subset_radius_x2 / 4.f;
				dp_norm_max += p_increment.vyy * p_increment.vyy * subset_radius_y2 * subset_radius_y2 / 4.f;
				dp_norm_max += p_increment.vxy * p_increment.vxy * subset_radius_xy;

				dp_norm_max = sqrt(dp_norm_max);
			} while (iteration < stop_condition && dp_norm_max >= conv_criterion);

			//store the final result
			poi->deformation.u = p_current.u;
			poi->deformation.ux = p_current.ux;
			poi->deformation.uy = p_current.uy;
			poi->deformation.uxx = p_current.uxx;
			poi->deformation.uyy = p_current.uyy;
			poi->deformation.uxy = p_current.uxy;

			poi->deformation.v = p_current.v;
			poi->deformation.vx = p_current.vx;
			poi->deformation.vy = p_current.vy;
			poi->deformation.vxx = p_current.vxx;
			poi->deformation.vyy = p_current.vyy;
			poi->deformation.vxy = p_current.vxy;

			//save the results for output
			poi->result.u0 = p_initial.u;
			poi->result.v0 = p_initial.v;
			poi->result.zncc = 0.5f * (2 - znssd);
			poi->result.iteration = (float)iteration;
			poi->result.convergence = dp_norm_max;
		}
	}

	void ICGN2D2::compute(std::vector<POI2D>& poi_queue) {
#pragma omp parallel for
		for (int i = 0; i < poi_queue.size(); ++i) {
			compute(&poi_queue[i]);
		}
	}


	//DVC, the 1st order shape function
	ICGN3D1_* ICGN3D1_::allocate(int subset_radius_x, int subset_radius_y, int subset_radius_z) {
		int dim_x = 2 * subset_radius_x + 1;
		int dim_y = 2 * subset_radius_y + 1;
		int dim_z = 2 * subset_radius_z + 1;
		Point3D subset_center(0, 0, 0);

		ICGN3D1_* ICGN_instance = new ICGN3D1_;
		ICGN_instance->ref_subset = new Subset3D(subset_center, subset_radius_x, subset_radius_y, subset_radius_z);
		ICGN_instance->tar_subset = new Subset3D(subset_center, subset_radius_x, subset_radius_y, subset_radius_z);
		ICGN_instance->error_img = new3D(dim_x, dim_y, dim_z);
		ICGN_instance->sd_img = new4D(dim_x, dim_y, dim_z, 12);

		return ICGN_instance;
	}

	void ICGN3D1_::release(ICGN3D1_* instance) {
		delete instance->ref_subset;
		delete instance->tar_subset;
		delete3D(instance->error_img);
		delete4D(instance->sd_img);
	}

	ICGN3D1_* ICGN3D1::getInstance(int tid) {
		if (tid >= (int)instance_pool.size()) {
			throw std::string("CPU thread ID over limit");
		}

		return instance_pool[tid];
	}

	ICGN3D1::ICGN3D1(int subset_radius_x, int subset_radius_y, int subset_radius_z, float conv_criterion, float stop_condition, int thread_number)
		: ref_gradient(nullptr), tar_interp(nullptr) {
		this->subset_radius_x = subset_radius_x;
		this->subset_radius_y = subset_radius_y;
		this->subset_radius_z = subset_radius_z;
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
		this->thread_number = thread_number;

		for (int i = 0; i < thread_number; i++) {
			ICGN3D1_* instance = ICGN3D1_::allocate(subset_radius_x, subset_radius_y, subset_radius_z);
			instance_pool.push_back(instance);
		}
	}

	ICGN3D1::~ICGN3D1() {
		delete ref_gradient;
		delete tar_interp;

		for (auto& instance : instance_pool) {
			ICGN3D1_::release(instance);
			delete instance;
		}
		instance_pool.clear();
	}

	void ICGN3D1::setIteration(float conv_criterion, float stop_condition) {
		this->conv_criterion = conv_criterion;
		this->stop_condition = stop_condition;
	}

	void ICGN3D1::setIteration(POI3D* poi) {
		conv_criterion = poi->result.convergence;
		stop_condition = (int)poi->result.iteration;
	}

	void ICGN3D1::prepareRef() {
		if (ref_gradient != nullptr) {
			delete ref_gradient;
			ref_gradient = nullptr;
		}

		ref_gradient = new Gradient3D4(*ref_img);
		ref_gradient->getGradientX();
		ref_gradient->getGradientY();
		ref_gradient->getGradientZ();
	}

	void ICGN3D1::prepareTar() {
		if (tar_interp != nullptr) {
			delete tar_interp;
			tar_interp = nullptr;
		}

		tar_interp = new TricubicBspline(*tar_img);
		tar_interp->prepare();
	}

	void ICGN3D1::prepare() {
		prepareRef();
		prepareTar();
	}

	void ICGN3D1::compute(POI3D* poi) {
		//set instance w.r.t. thread id 
		ICGN3D1_* cur_instance = getInstance(omp_get_thread_num());

		if ((poi->x - subset_radius_x) < 0 || (poi->y - subset_radius_y) < 0 || (poi->z - subset_radius_z) < 0
			|| (poi->x + subset_radius_x) > (ref_img->dim_x - 1) || (poi->y + subset_radius_y) > (ref_img->dim_y - 1)
			|| (poi->z + subset_radius_z) > (ref_img->dim_z - 1)) {
			//std::cerr << "POI is too close to boundary" << poi->x << ", " << poi->y << ", " << poi->z << std::endl;
			poi->result.zncc = -1;
		}
		else {
			int subset_dim_x = 2 * subset_radius_x + 1;
			int subset_dim_y = 2 * subset_radius_y + 1;
			int subset_dim_z = 2 * subset_radius_z + 1;

			//set reference subset
			cur_instance->ref_subset->center = (Point3D)*poi;
			cur_instance->ref_subset->fill(ref_img);
			float ref_mean_norm = cur_instance->ref_subset->zeroMeanNorm();

			//build the hessian matrix
			cur_instance->hessian.setZero();
			for (int i = 0; i < subset_dim_z; i++) {
				for (int j = 0; j < subset_dim_y; j++) {
					for (int k = 0; k < subset_dim_x; k++) {
						int x_local = k - subset_radius_x;
						int y_local = j - subset_radius_y;
						int z_local = i - subset_radius_z;
						int x_global = (int)poi->x + x_local;
						int y_global = (int)poi->y + y_local;
						int z_global = (int)poi->z + z_local;
						float ref_gradient_x = ref_gradient->gradient_x[z_global][y_global][x_global];
						float ref_gradient_y = ref_gradient->gradient_y[z_global][y_global][x_global];
						float ref_gradient_z = ref_gradient->gradient_z[z_global][y_global][x_global];

						cur_instance->sd_img[i][j][k][0] = ref_gradient_x;
						cur_instance->sd_img[i][j][k][1] = ref_gradient_x * x_local;
						cur_instance->sd_img[i][j][k][2] = ref_gradient_x * y_local;
						cur_instance->sd_img[i][j][k][3] = ref_gradient_x * z_local;
						cur_instance->sd_img[i][j][k][4] = ref_gradient_y;
						cur_instance->sd_img[i][j][k][5] = ref_gradient_y * x_local;
						cur_instance->sd_img[i][j][k][6] = ref_gradient_y * y_local;
						cur_instance->sd_img[i][j][k][7] = ref_gradient_y * z_local;
						cur_instance->sd_img[i][j][k][8] = ref_gradient_z;
						cur_instance->sd_img[i][j][k][9] = ref_gradient_z * x_local;
						cur_instance->sd_img[i][j][k][10] = ref_gradient_z * y_local;
						cur_instance->sd_img[i][j][k][11] = ref_gradient_z * z_local;

						for (int r = 0; r < 12; r++) {
							for (int c = 0; c < 12; c++) {
								cur_instance->hessian(r, c) += (cur_instance->sd_img[i][j][k][r] * cur_instance->sd_img[i][j][k][c]);
							}
						}
					}
				}
			}
			//compute inversed hessian matrix
			cur_instance->inv_hessian = cur_instance->hessian.inverse();

			//set target subset
			cur_instance->tar_subset->center = (Point3D)*poi;

			//get initial guess
			Deformation3D1 p_initial(poi->deformation.u, poi->deformation.ux, poi->deformation.uy, poi->deformation.uz,
				poi->deformation.v, poi->deformation.vx, poi->deformation.vy, poi->deformation.vz,
				poi->deformation.w, poi->deformation.wx, poi->deformation.wy, poi->deformation.wz);

			//IC-GN iteration
			int iteration = 0; //initialize iteration counter
			Deformation3D1 p_current, p_increment;
			p_current.setDeformation(p_initial);
			float dp_norm_max, znssd;
			Point3D local_coor, warped_coor, global_coor;
			do {
				iteration++;
				//reconstruct target subset
				for (int i = 0; i < subset_dim_z; i++) {
					for (int j = 0; j < subset_dim_y; j++) {
						for (int k = 0; k < subset_dim_x; k++) {
							int x_local = k - subset_radius_x;
							int y_local = j - subset_radius_y;
							int z_local = i - subset_radius_z;
							local_coor.x = x_local;
							local_coor.y = y_local;
							local_coor.z = z_local;
							warped_coor = p_current.warp(local_coor);
							global_coor = cur_instance->tar_subset->center + warped_coor;
							cur_instance->tar_subset->vol_mat[i][j][k] = tar_interp->compute(global_coor);
						}
					}
				}
				float tar_mean_norm = cur_instance->tar_subset->zeroMeanNorm();

				//compute error image
				float error_factor = ref_mean_norm / tar_mean_norm;
				float squared_sum = 0;
				for (int i = 0; i < subset_dim_z; i++) {
					for (int j = 0; j < subset_dim_y; j++) {
						for (int k = 0; k < subset_dim_x; k++) {
							cur_instance->error_img[i][j][k] = error_factor * cur_instance->tar_subset->vol_mat[i][j][k] - cur_instance->ref_subset->vol_mat[i][j][k];
							squared_sum += (cur_instance->error_img[i][j][k] * cur_instance->error_img[i][j][k]);
						}
					}
				}

				//calculate ZNSSD
				znssd = squared_sum / (ref_mean_norm * ref_mean_norm);

				//compute numerator
				float numerator[12] = { 0 };
				for (int i = 0; i < subset_dim_z; i++) {
					for (int j = 0; j < subset_dim_y; j++) {
						for (int k = 0; k < subset_dim_x; k++) {
							for (int l = 0; l < 12; l++) {
								numerator[l] += (cur_instance->sd_img[i][j][k][l] * cur_instance->error_img[i][j][k]);
							}
						}
					}
				}

				//compute dp
				float dp[12] = { 0 };
				for (int i = 0; i < 12; i++) {
					for (int j = 0; j < 12; j++) {
						dp[i] += (cur_instance->inv_hessian(i, j) * numerator[j]);
					}
				}
				p_increment.setDeformation(dp);

				//update warp
				p_current.warp_matrix = p_current.warp_matrix * p_increment.warp_matrix.inverse();

				//update p
				p_current.setDeformation();

				//check convergence
				dp_norm_max = sqrt(p_increment.u * p_increment.u + p_increment.v * p_increment.v + p_increment.w * p_increment.w);

			} while (iteration < stop_condition && dp_norm_max >= conv_criterion);

			//store the final results
			poi->deformation.u = p_current.u;
			poi->deformation.ux = p_current.ux;
			poi->deformation.uy = p_current.uy;
			poi->deformation.uz = p_current.uz;
			poi->deformation.v = p_current.v;
			poi->deformation.vx = p_current.vx;
			poi->deformation.vy = p_current.vy;
			poi->deformation.vz = p_current.vz;
			poi->deformation.w = p_current.w;
			poi->deformation.wx = p_current.wx;
			poi->deformation.wy = p_current.wy;
			poi->deformation.wz = p_current.wz;

			//save the results for output
			poi->result.u0 = p_initial.u;
			poi->result.v0 = p_initial.v;
			poi->result.w0 = p_initial.w;
			poi->result.zncc = 0.5f * (2 - znssd);
			poi->result.iteration = (float)iteration;
			poi->result.convergence = dp_norm_max;
		}
	}

	void ICGN3D1::compute(std::vector<POI3D>& poi_queue) {
#pragma omp parallel for
		for (int i = 0; i < poi_queue.size(); ++i) {
			compute(&poi_queue[i]);
		}
	}

}//namespace opencorr