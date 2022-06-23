/*
 This example demonstrates how to use OpenCorr to realize the path independent
 DIC based on FFT-CC algorithm and ICGN algorithm (with the 1st order shape
 function).
*/

#include "opencorr.h"

using namespace opencorr;
using namespace std;

int main() {
	//set files to process
	string ref_image_path = "../samples/oht_cfrp_0.bmp";
	string tar_image_path = "../samples/oht_cfrp_4.bmp";
	Image2D ref_img(ref_image_path);
	Image2D tar_img(tar_image_path);

	//get time of start
	double timer_tic = omp_get_wtime();

	//Initialization of data structure and parameters
	//set OpenMP parameters
	int cpu_thread_number = omp_get_num_procs() - 1;
	omp_set_num_threads(cpu_thread_number);

	//set DIC parameters
	int subset_radius_x = 16;
	int subset_radius_y = 16;
	int max_iteration = 10;
	float max_deformation_norm = 0.001f;

	//set POIs
	Point2D upper_left_point(30, 30);
	vector<POI2D> poi_queue;
	int poi_number_x = 100;
	int poi_number_y = 300;
	int grid_space = 2;

	//store POIs in a queue
	for (int i = 0; i < poi_number_y; i++) {
		for (int j = 0; j < poi_number_x; j++) {
			Point2D offset(j * grid_space, i * grid_space);
			Point2D current_point = upper_left_point + offset;
			POI2D current_poi(current_point);
			poi_queue.push_back(current_poi);
		}
	}

	//get the time of end 
	double timer_toc = omp_get_wtime();
	double consumed_time = timer_toc - timer_tic;

	//display the time of initialization on the screen
	cout << "Initialization: " << consumed_time << " sec" << std::endl;

	//get time of start
	timer_tic = omp_get_wtime();

	//FFTCC
	FFTCC2D* fftcc = new FFTCC2D(subset_radius_x, subset_radius_y, cpu_thread_number);
	fftcc->setImages(ref_img, tar_img);
	fftcc->compute(poi_queue);

	//get the time of end 
	timer_toc = omp_get_wtime();
	consumed_time = timer_toc - timer_tic;

	//display the time of FFT-CC on the screen
	cout << "FFT-CC: " << consumed_time << " sec" << std::endl;

	//get time of start
	timer_tic = omp_get_wtime();

	//ICGN with the 1st order shape function
	ICGN2D1* icgn1 = new ICGN2D1(subset_radius_x, subset_radius_y, max_deformation_norm, max_iteration, cpu_thread_number);
	icgn1->setImages(ref_img, tar_img);
	icgn1->prepare();
	icgn1->compute(poi_queue);

	//get the time of end 
	timer_toc = omp_get_wtime();
	consumed_time = timer_toc - timer_tic;

	//display the time of processing on the screen
	cout << "ICGN: " << consumed_time << " sec" << std::endl;

	//output the results
	IO2D results_output;
	string file_path;
	results_output.setHeight(ref_img.height);
	results_output.setWidth(ref_img.width);
	results_output.setDelimiter(string(","));

	//save the calculated dispalcements
	file_path = tar_image_path + "_fftcc_icgn1_table.csv";
	results_output.setPath(file_path);
	results_output.saveTable2D(poi_queue);

	//save the full deformation vector
	file_path = tar_image_path + "_fftcc_icgn1_deformation.csv";
	results_output.setPath(file_path);
	results_output.saveDeformationTable2D(poi_queue);

	//save the map of u-component
	file_path = tar_image_path + "_fftcc_icgn1_u.csv";
	char var_char = 'u';
	results_output.setPath(file_path);
	results_output.saveMap2D(poi_queue, var_char);

	//save the map of v-component
	file_path = tar_image_path + "_fftcc_icgn1_v.csv";
	var_char = 'v';
	results_output.setPath(file_path);
	results_output.saveMap2D(poi_queue, var_char);

	cout << "Press any key to exit" << std::endl;
	cin.get();

	delete icgn1;
	delete fftcc;

	return 0;
}
