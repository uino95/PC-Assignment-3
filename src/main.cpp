
#include <getopt.h>
#include <iostream>
#include <cmath>
#include <string>
#include "utilities/lodepng.h"
#include "utilities/rgba.hpp"
#include "utilities/num.hpp"
#include <complex>
#include <cassert>
#include <limits>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

using namespace std;

static std::vector<std::pair<double,rgb>> colourGradient = {
	{ 0.0		, { 0  , 0  , 0   } },
	{ 0.03		, { 0  , 7  , 100 } },
	{ 0.16		, { 32 , 107, 203 } },
	{ 0.42		, { 237, 255, 255 } },
	{ 0.64		, { 255, 170, 0   } },
	{ 0.86		, { 0  , 2  , 0   } },
	{ 1.0		, { 0  , 0  , 0   } }
};

static unsigned int blockDim = 16;
static unsigned int subDiv = 4;

static unsigned int res = 1024;
static unsigned int maxDwell = 512;
static bool mark = false;

static constexpr const int  dwellFill = std::numeric_limits<int>::max();
static constexpr const int  dwellCompute = std::numeric_limits<int>::max()-1;
static constexpr const rgba borderFill(255,255,255,255);
static constexpr const rgba borderCompute(255,0,0,255);
static std::vector<rgba> colours;

std::mutex mutexVariable;

void createColourMap(unsigned int const maxDwell) {
	rgb colour(0,0,0);
	double pos = 0.0;

	//Adding the last value if its not there...
	if (colourGradient.size() == 0 || colourGradient.back().first != 1.0) {
		colourGradient.push_back({ 1.0, {0,0,0}});
	}

	for (auto &gradient : colourGradient) {
		int r = (int) gradient.second.r - colour.r;
		int g = (int) gradient.second.g - colour.g;
		int b = (int) gradient.second.b - colour.b;
		unsigned int const max = std::ceil((double) maxDwell * (gradient.first - pos));
		for (unsigned int i = 0; i < max; i++) {
			double blend = (double) i / max;
			rgba newColour(
				colour.r + (blend * r),
				colour.g + (blend * g),
				colour.b + (blend * b),
				255
			);
			colours.push_back(newColour);
		}
		pos = gradient.first;
		colour = gradient.second;
	}
}



rgba const &dwellColor(std::complex<double> const z, unsigned int const dwell) {
	static constexpr const double log2 = 0.693147180559945309417232121458176568075500134360255254120;
	assert(colours.size() > 0);
	switch (dwell) {
		case dwellFill:
			return borderFill;
		case dwellCompute:
			return borderCompute;
	}
	unsigned int index = dwell + 1 - std::log(std::log(std::abs(z))/log2);
	return colours.at(index % colours.size());
}

unsigned int pixelDwell(std::complex<double> const &cmin,
						std::complex<double> const &dc,
						unsigned int const y,
						unsigned int const x)
{
	double const fy = (double)y / res;
	double const fx = (double)x / res;
	std::complex<double> const c = cmin + std::complex<double>(fx * dc.real(), fy * dc.imag());
	std::complex<double> z = c;
	unsigned int dwell = 0;

	while(dwell < maxDwell && std::abs(z) < (2 * 2)) {
		z = z * z + c;
		dwell++;
	}

	return dwell;
}

int commonBorder(std::vector<std::vector<int>> &dwellBuffer,
				 std::complex<double> const &cmin,
				 std::complex<double> const &dc,
				 unsigned int const atY,
				 unsigned int const atX,
				 unsigned int const blockSize)
{
	unsigned int const yMax = (res > atY + blockSize - 1) ? atY + blockSize - 1 : res - 1;
	unsigned int const xMax = (res > atX + blockSize - 1) ? atX + blockSize - 1 : res - 1;
	int commonDwell = -1;
	for (unsigned int i = 0; i < blockSize; i++) {
		for (unsigned int s = 0; s < 4; s++) {
			unsigned const int y = s % 2 == 0 ? atY + i : (s == 1 ? yMax : atY);
			unsigned const int x = s % 2 != 0 ? atX + i : (s == 0 ? xMax : atX);
			if (y < res && x < res) {
				if (dwellBuffer.at(y).at(x) < 0) {
					dwellBuffer.at(y).at(x) = pixelDwell(cmin, dc, y, x);
				}
				if (commonDwell == -1) {
					commonDwell = dwellBuffer.at(y).at(x);
				} else if (commonDwell != dwellBuffer.at(y).at(x)) {
					return -1;
				}
			}
		}
	}
	return commonDwell;
}

/**
* commonBorder function that computes the commonDwell while more threads concurrently update it.
* For this reason we added a mutex variable that prevents the update of the commonDwell variable simultaneously.
* The return -1 can't be here, as we are no more inside the loop as before, so we set in that case commonDwell to -2,
* and then consider this case in the multipleThreadCommonBorder function as the case when we must exit the loop.
*/
void threadedCommonBorder(
	unsigned int i,
	unsigned int s,
	unsigned int yMax,
	unsigned int xMax,
	unsigned int atY,
	unsigned int atX,
	std::vector<std::vector<int>> &dwellBuffer,
	int& commonDwell,
	std::complex<double> const &cmin,
	std::complex<double> const &dc
) {
	unsigned const int y = s % 2 == 0 ? atY + i : (s == 1 ? yMax : atY);
	unsigned const int x = s % 2 != 0 ? atX + i : (s == 0 ? xMax : atX);
	if (y < res && x < res) {
		if (dwellBuffer.at(y).at(x) < 0) {
			dwellBuffer.at(y).at(x) = pixelDwell(cmin, dc, y, x);
		}
		mutexVariable.lock();

		if (commonDwell == -1) {
			commonDwell = dwellBuffer.at(y).at(x);
		} else if (commonDwell != dwellBuffer.at(y).at(x)) {
			commonDwell = -2;
		}

		mutexVariable.unlock();
	}
}

/**
* Parallelized version. At most 4 threads are executed in parallel, so to compute the common border.
*/
int multipleThreadCommonBorder(std::vector<std::vector<int>> &dwellBuffer,
				 std::complex<double> const &cmin,
				 std::complex<double> const &dc,
				 unsigned int const atY,
				 unsigned int const atX,
				 unsigned int const blockSize)
{
	unsigned int const yMax = (res > atY + blockSize - 1) ? atY + blockSize - 1 : res - 1;
	unsigned int const xMax = (res > atX + blockSize - 1) ? atX + blockSize - 1 : res - 1;
	int commonDwell = -1;
	for (unsigned int i = 0; i < blockSize; i++) {
		vector<thread> threads;
		for (unsigned int s = 0; s < 4; s++) {
			threads.push_back(
				thread(
					threadedCommonBorder,
					i,
					s,
					yMax,
					xMax,
					atY,
					atX,
					ref(dwellBuffer),
					ref(commonDwell),
					cmin,
					dc
				)
			);
		}
		for(unsigned int s = 0; s < 4; s++) {
			threads.at(s).join();
		}

		if(commonDwell == -2) {
			return commonDwell;
		}
	}

	return commonDwell;
}

void markBorder(std::vector<std::vector<int>> &dwellBuffer,
				int const dwell,
				unsigned int const atY,
				unsigned int const atX,
				unsigned int const blockSize)
{
	unsigned int const yMax = (res > atY + blockSize - 1) ? atY + blockSize - 1 : res - 1;
	unsigned int const xMax = (res > atX + blockSize - 1) ? atX + blockSize - 1 : res - 1;
	//#pragma omp parallel for
	for (unsigned int i = 0; i < blockSize; i++) {
		//#pragma omp parallel for
		for (unsigned int s = 0; s < 4; s++) {
			unsigned const int y = s % 2 == 0 ? atY + i : (s == 1 ? yMax : atY);
			unsigned const int x = s % 2 != 0 ? atX + i : (s == 0 ? xMax : atX);
			if (y < res && x < res) {
				dwellBuffer.at(y).at(x) = dwell;
			}
		}
	}
}

void computeBlock(std::vector<std::vector<int>> &dwellBuffer,
	std::complex<double> const &cmin,
	std::complex<double> const &dc,
	unsigned int const atY,
	unsigned int const atX,
	unsigned int const blockSize,
	unsigned int const omitBorder = 0)
{
	unsigned int const yMax = (res > atY + blockSize) ? atY + blockSize : res;
	unsigned int const xMax = (res > atX + blockSize) ? atX + blockSize : res;
	for (unsigned int y = atY + omitBorder; y < yMax - omitBorder; y++) {
		for (unsigned int x = atX + omitBorder; x < xMax - omitBorder; x++) {
			dwellBuffer.at(y).at(x) = pixelDwell(cmin, dc, y, x);
		}
	}
}

/**
* Parallelized version. Only changes the yMax
*/
void threadedComputeBlock(std::vector<std::vector<int>> &dwellBuffer,
	std::complex<double> const &cmin,
	std::complex<double> const &dc,
	unsigned int const atY,
	unsigned int const atX,
	unsigned int const blockSize,
	unsigned int const omitBorder = 0)
{
	unsigned int const yMax = (res > atY + blockSize) ? atY + blockSize : res;
	unsigned int const xMax = res;
	for (unsigned int y = atY + omitBorder; y < yMax - omitBorder; y++) {
		for (unsigned int x = atX + omitBorder; x < xMax - omitBorder; x++) {
			dwellBuffer.at(y).at(x) = pixelDwell(cmin, dc, y, x);
		}
	}
}

void fillBlock(std::vector<std::vector<int>> &dwellBuffer,
			   int const dwell,
			   unsigned int const atY,
			   unsigned int const atX,
			   unsigned int const blockSize,
			   unsigned int const omitBorder = 0)
{
	unsigned int const yMax = (res > atY + blockSize) ? atY + blockSize : res;
	unsigned int const xMax = (res > atX + blockSize) ? atX + blockSize : res;
	for (unsigned int y = atY + omitBorder; y < yMax - omitBorder; y++) {
		for (unsigned int x = atX + omitBorder; x < xMax - omitBorder; x++) {
			if (dwellBuffer.at(y).at(x) < 0) {
				dwellBuffer.at(y).at(x) = dwell;
			}
		}
	}
}


// define job data type here
typedef struct job {
   std::vector<std::vector<int>> &dwellBuffer;
   int dwell;
   unsigned int atY;
   unsigned int atX;
   unsigned int blockSize;
	 std::complex<double> dc;
	 std::complex<double> cmin;
} job;

// define mutex, condition variable, atomic variables and deque here
std::deque<job> queue;
std::mutex mutexVariable2;
std::condition_variable myCv;
atomic<int> counter(0), limit(0);

void addWork(job task)
{
	unique_lock<mutex> lck(mutexVariable2);
	queue.push_back(task);
	myCv.notify_all();
}

// Original version of marianiSilver algorithm
void marianiSilverOriginal( std::vector<std::vector<int>> &dwellBuffer,
					std::complex<double> const &cmin,
					std::complex<double> const &dc,
					unsigned int const atY,
					unsigned int const atX,
					unsigned int const blockSize)
{
	int dwell = commonBorder(dwellBuffer, cmin, dc, atY, atX, blockSize);
	if ( dwell >= 0 ) {
		fillBlock(dwellBuffer, dwell, atY, atX, blockSize);
		if (mark) {
					markBorder(dwellBuffer, dwellFill, atY, atX, blockSize);
		}
	} else if (blockSize <= blockDim) {
		computeBlock(dwellBuffer, cmin, dc, atY, atX, blockSize);
		if (mark)
			markBorder(dwellBuffer, dwellCompute, atY, atX, blockSize);
	} else {
		// Subdivision
		unsigned int newBlockSize = blockSize / subDiv;
		for (unsigned int ydiv = 0; ydiv < subDiv; ydiv++) {
			for (unsigned int xdiv = 0; xdiv < subDiv; xdiv++) {
				marianiSilverOriginal(dwellBuffer, cmin, dc, atY + (ydiv * newBlockSize), atX + (xdiv * newBlockSize), newBlockSize);
			}
		}
	}
}

/**
* Task 1b: computation of the dwell is parallelized.
*/
void marianiSilverWithThreadedCommonBorder( std::vector<std::vector<int>> &dwellBuffer,
					std::complex<double> const &cmin,
					std::complex<double> const &dc,
					unsigned int const atY,
					unsigned int const atX,
					unsigned int const blockSize)
{
	int dwell = multipleThreadCommonBorder(dwellBuffer, cmin, dc, atY, atX, blockSize);
	if ( dwell >= 0 ) {
		fillBlock(dwellBuffer, dwell, atY, atX, blockSize);
		if (mark) {
					markBorder(dwellBuffer, dwellFill, atY, atX, blockSize);
		}
	} else if (blockSize <= blockDim) {
		computeBlock(dwellBuffer, cmin, dc, atY, atX, blockSize);
		if (mark)
			markBorder(dwellBuffer, dwellCompute, atY, atX, blockSize);
	} else {
		// Subdivision
		unsigned int newBlockSize = blockSize / subDiv;
		for (unsigned int ydiv = 0; ydiv < subDiv; ydiv++) {
			for (unsigned int xdiv = 0; xdiv < subDiv; xdiv++) {
				marianiSilverWithThreadedCommonBorder(dwellBuffer, cmin, dc, atY + (ydiv * newBlockSize), atX + (xdiv * newBlockSize), newBlockSize);
			}
		}
	}
}

/**
* Task 1c: parallelized version with recursion
*/
void marianiSilver( std::vector<std::vector<int>> &dwellBuffer,
					std::complex<double> const &cmin,
					std::complex<double> const &dc,
					unsigned int const atY,
					unsigned int const atX,
					unsigned int const blockSize)
{
	int dwell = commonBorder(dwellBuffer, cmin, dc, atY, atX, blockSize);
	if ( dwell >= 0 ) {
		fillBlock(dwellBuffer, dwell, atY, atX, blockSize);
		if (mark) {
					markBorder(dwellBuffer, dwellFill, atY, atX, blockSize);
		}
	} else if (blockSize <= blockDim) {
		computeBlock(dwellBuffer, cmin, dc, atY, atX, blockSize);
		if (mark)
			markBorder(dwellBuffer, dwellCompute, atY, atX, blockSize);
	} else {
		// Subdivision
		unsigned int newBlockSize = blockSize / subDiv;
		vector<thread> threads;
		for (unsigned int ydiv = 0; ydiv < subDiv; ydiv++) {
			for (unsigned int xdiv = 0; xdiv < subDiv; xdiv++) {
				threads.push_back(
					thread(
						marianiSilver,

						ref(dwellBuffer),
						cmin,
						dc,
						atY + (ydiv * newBlockSize),
						atX + (xdiv * newBlockSize),
						newBlockSize
					)
				);
			}
		}

		for(unsigned int i=0;i<threads.size(); i++) {
			threads.at(i).join();
		}
	}
}

/**
* Task 2
* Instead of calling recursively the marianiSilver, we add a job into the queue.
*/
void marianiSilverJob( std::vector<std::vector<int>> &dwellBuffer,
					std::complex<double> const &cmin,
					std::complex<double> const &dc,
					unsigned int const atY,
					unsigned int const atX,
					unsigned int const blockSize)
{

	int dwell = commonBorder(dwellBuffer, cmin, dc, atY, atX, blockSize);
	if ( dwell >= 0 ) {
		fillBlock(dwellBuffer, dwell, atY, atX, blockSize);
		if (mark) {
					markBorder(dwellBuffer, dwellFill, atY, atX, blockSize);
		}
	} else if (blockSize <= blockDim) {
		computeBlock(dwellBuffer, cmin, dc, atY, atX, blockSize);
		if (mark)
			markBorder(dwellBuffer, dwellCompute, atY, atX, blockSize);
	} else {
		// Update the total number of job to execute
		limit += subDiv * subDiv;
		// Subdivision
		unsigned int newBlockSize = blockSize / subDiv;
		for (unsigned int ydiv = 0; ydiv < subDiv; ydiv++) {
			for (unsigned int xdiv = 0; xdiv < subDiv; xdiv++) {
				addWork(
					job{
						dwellBuffer,
				    dwell,
				    atY + (ydiv * newBlockSize),
						atX + (xdiv * newBlockSize),
						newBlockSize,
						dc,
						cmin
					}
				);
			}
		}
	}
}

void help() {
	std::cout << "Mandelbrot Set Renderer" << std::endl;
	std::cout << std::endl;
	std::cout << "\t" << "-x [0;1]" << "\t" << "Center of Re[-1.5;0.5] (default=0.5)" << std::endl;
	std::cout << "\t" << "-y [0;1]" << "\t" << "Center of Im[-1;1] (default=0.5)" << std::endl;
	std::cout << "\t" << "-s (0;1]" << "\t" << "Inverse scaling factor (default=1)" << std::endl;
	std::cout << "\t" << "-r [pixel]" << "\t" << "Image resolution (default=1024)" << std::endl;
	std::cout << "\t" << "-i [iterations]" << "\t" << "Iterations or max dwell (default=512)" << std::endl;
	std::cout << "\t" << "-c [colours]" << "\t" << "colour map iterations (default=1)" << std::endl;
	std::cout << "\t" << "-b [block dim]" << "\t" << "min block dimension for subdivision (default=16)" << std::endl;
	std::cout << "\t" << "-d [subdivison]" << "\t" << "subdivision of blocks (default=4)" << std::endl;
	std::cout << "\t" << "-m" << "\t" << "mark Mariani-Silver borders" << std::endl;
	std::cout << "\t" << "-t" << "\t" << "traditional computation (no Mariani-Silver)" << std::endl;
}

// Multiple thread version for task 2c
void worker(std::vector<std::vector<int>> &dwellBuffer) {

	// Initialize an empty job
	job currentTask{
		dwellBuffer,0,0,0,0,NULL,NULL
	};

	// Continue until there is work to do
	while(counter < limit) {
		// Scope of mutexVariable2
		{
			// Acquire the lock on mutexVariable2
			unique_lock<mutex> lck(mutexVariable2);
			// If the queue is empty wait until a new job it's available
			while(queue.empty() && counter < limit) {
				myCv.wait(lck);
			}
			// If there is work to do just pop it from the queue
			if(counter < limit){
				currentTask.dwellBuffer = queue.front().dwellBuffer;
				currentTask.cmin = queue.front().cmin;
				currentTask.dc = queue.front().dc;
				currentTask.atX = queue.front().atX;
				currentTask.atY = queue.front().atY;
				currentTask.blockSize = queue.front().blockSize;
				queue.pop_front();
				counter++;
			}
		// Unlock mutexVariable2
		}
		// Execute the actual work
		marianiSilverJob(currentTask.dwellBuffer, currentTask.cmin, currentTask.dc, currentTask.atY, currentTask.atX, currentTask.blockSize);
	}
}

// Single thread worker function for task 2a
void workerWithoutThread(void){
	while(!queue.empty()){
		job currentTask = queue.front();
		queue.pop_front();
		marianiSilverJob(currentTask.dwellBuffer, currentTask.cmin, currentTask.dc, currentTask.atY, currentTask.atX, currentTask.blockSize);
	}
}

int main( int argc, char *argv[] )
{
	std::string output = "output.png";
	double x = 0.5, y = 0.5;
	double scale = 1;
	unsigned int colourIterations = 1;
	bool mariani = true;
	bool quiet = false;

	{
		char c;
		while((c = getopt(argc,argv,"x:y:s:r:o:i:c:b:d:mthq"))!=-1) {
			switch(c) {
				case 'x':
					x = num::clamp(atof(optarg),0.0,1.0);
					break;
				case 'y':
					y = num::clamp(atof(optarg),0.0,1.0);
					break;
				case 's':
					scale = num::clamp(atof(optarg),0.0,1.0);
					if (scale == 0) scale = 1;
					break;
				case 'r':
					res = std::max(1,atoi(optarg));
					break;
				case 'i':
					maxDwell = std::max(1,atoi(optarg));
					break;
				case 'c':
					colourIterations = std::max(1,atoi(optarg));
					break;
				case 'b':
					blockDim = std::max(4,atoi(optarg));
					break;
				case 'd':
					subDiv = std::max(2,atoi(optarg));
					break;
				case 'm':
					mark = true;
					break;
				case 't':
					mariani = false;
					break;
				case 'q':
					quiet = true;
					break;
				case 'o':
					output = optarg;
					break;
				case 'h':
					help();
					exit(0);
					break;
				default:
					std::cerr << "Unknown argument '" << c << "'" << std::endl << std::endl;
					help();
					exit(1);
			}
		}
	}

	double const xmin = -3.5 + (2 * 2 * x);
	double const xmax = -1.5 + (2 * 2 * x);
	double const ymin = -3.0 + (2 * 2 * y);
	double const ymax = -1.0 + (2 * 2 * y);
	double const xlen = std::abs(xmin - xmax);
	double const ylen = std::abs(ymin - ymax);

	std::complex<double> const cmin(xmin + (0.5 * (1 - scale) * xlen),ymin + (0.5 * (1 - scale) * ylen));
	std::complex<double> const cmax(xmax - (0.5 * (1 - scale) * xlen),ymax - (0.5 * (1 - scale) * ylen));
	std::complex<double> const dc = cmax - cmin;

	if (!quiet) {
		std::cout << std::fixed;
		std::cout << "Center:      [" << x << "," << y << "]" << std::endl;
		std::cout << "Zoom:        " << (unsigned long long) (1/scale) * 100 << "%" <<  std::endl;
		std::cout << "Iterations:  " << maxDwell  << std::endl;
		std::cout << "Window:      Re[" << cmin.real() << ", " << cmax.real() << "], Im[" << cmin.imag() << ", " << cmax.imag() << "]" << std::endl;
		std::cout << "Output:      " << output << std::endl;
		std::cout << "Block dim:   " << blockDim << std::endl;
		std::cout << "Subdivision: " << subDiv << std::endl;
		std::cout << "Borders:     " << ((mark) ? "marking" : "not marking") << std::endl;
	}

	std::vector<std::vector<int>> dwellBuffer(res, std::vector<int>(res, -1));
	vector<thread> threads;
	unsigned int const NUM_THREAD = thread::hardware_concurrency();


	if (mariani) {
		// Scale the blockSize from res up to a subdividable value
		// Number of possible subdivisions:
		unsigned int const numDiv = std::ceil(std::log((double) res/blockDim)/std::log((double) subDiv));
		// Calculate a dividable resolution for the blockSize:
		unsigned int const correctedBlockSize = std::pow(subDiv,numDiv) * blockDim;
		// Mariani-Silver subdivision algorithm

		//addWork(job{dwellBuffer, 0, 0, 0, correctedBlockSize, dc, cmin});
		// Initialize the variable to 1 in order to execute the first step
		//limit = 1;

		// Initialize the vector of threads and make them execute the worker function
		for(unsigned int i=0;i<NUM_THREAD; i++) {
			threads.push_back(
				thread(
					worker,
					ref(dwellBuffer)
				)
			);
		}

		// Wait for all the thread to finish
		for(unsigned int i=0;i<NUM_THREAD; i++) {
			threads.at(i).join();
		}

		//Call to the original implementation of mariani silver
		//marianiSilverOriginal(dwellBuffer, cmin, dc, 0, 0, correctedBlockSize);

		//Call to the parallelized version of mariani silver
		//marianiSilver(dwellBuffer, cmin, dc, 0, 0, correctedBlockSize);
	} else {
		// Traditional Mandelbrot-Set computation or the 'Escape Time' algorithm.
		//implementation is now threaded
		unsigned int const HEIGHT_PER_THREAD = res / NUM_THREAD;

		// Initialize the vector of threads and make them execute the threadedComputeBlock function
		for(unsigned int i=0;i<NUM_THREAD; i++) {
			threads.push_back(

				thread(
					threadedComputeBlock,

					ref(dwellBuffer),
					cmin,
					dc,
					HEIGHT_PER_THREAD * i,
					0,
					HEIGHT_PER_THREAD,0
				)
			);
		}

		// Wait for all the thread to finish
		for(unsigned int i=0;i<NUM_THREAD; i++) {
			threads.at(i).join();
		}

		if (mark)
			markBorder(dwellBuffer, dwellCompute, 0, 0, res);
	}

	// Add here the worker for Task 2

	// The colour iterations defines how often the colour gradient will
	// be seen on the final picture. Basically the repetitive factor
	createColourMap(maxDwell / colourIterations);
	std::vector<unsigned char> frameBuffer(res * res * 4, 0);
	unsigned char *pixel = &(frameBuffer.at(0));

	// Map the dwellBuffer to the frameBuffer
	for (unsigned int y = 0; y < res; y++) {
		for (unsigned int x = 0; x < res; x++) {
			// Getting a colour from the map depending on the dwell value and
			// the coordinates as a complex number. This  method is responsible
			// for all the nice colours you see
			rgba const &colour = dwellColor(std::complex<double>(x,y), dwellBuffer.at(y).at(x));
			// class rgba provides a method to directly write a colour into a
			// framebuffer. The address to the next pixel is hereby returned
			pixel = colour.putFramebuffer(pixel);
		}
	}

	unsigned int const error = lodepng::encode(output, frameBuffer, res, res);
	if (error) {
		std::cout << "An error occurred while writing the image file: " << error << ": " << lodepng_error_text(error) << std::endl;
		return 1;
	}

	return 0;
}
