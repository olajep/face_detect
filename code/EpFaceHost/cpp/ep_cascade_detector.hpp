/* <title of the code in this file>
   Copyright (C) 2012 Adapteva, Inc.
   
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program, see the file COPYING.  If not, see
   <http://www.gnu.org/licenses/>. */

/**
 * C++ interface for Adapteva implementation of LBP face detection algorithm
 */
#ifndef EP_CASCADE_DETECTOR_HPP
#define EP_CASCADE_DETECTOR_HPP

#include <string>
#include <vector>

#include <opencv2/core/core.hpp>

//NOTE: comment/uncomment following #include to disable/enable integration with OpenCV ObjDetect module.
//In case of enabled integration please provide correct cascadedetect.hpp file from OpenCV version you use.

#include <opencv2/objdetect/objdetect.hpp>

#include "../c/ep_cascade_detector.h"

namespace ep {
/**
 * This is classifier usable by detect_multi_scale function.
 * Can be converted from cv::CascadeClassifier.
 * Wrapper around EpCascadeClassifier
 */
class CascadeClassifier {
public:
    CascadeClassifier(void);
    /// Load classifier from data file
    CascadeClassifier(std::string const &file_name);
#ifdef __OPENCV_OBJDETECT_HPP__
    /// Create cascade from OpenCV classifier
    CascadeClassifier(cv::CascadeClassifier const &cv_classifier);
    /// Assign OpenCV classifier to ep::CascadeClassifier
    CascadeClassifier &operator=(cv::CascadeClassifier const &cv_classifier);
#endif

    /// Copy constructor
    CascadeClassifier(CascadeClassifier const &classifier);

    /// Destructor
    ~CascadeClassifier(void);

    /// Assignment operator
    CascadeClassifier &operator=(CascadeClassifier const &classifier);

    /// Determine whether classifier is empty
    bool empty(void) const;

    /// Release classifier data
    void release(void);

    /// Load classifier contents from file
    EpErrorCode load(std::string const &file_name);

    /// Save classifier contents to binary file
    EpErrorCode save(std::string const &file_name) const;

    /// Get classifier data usable by C function ep_detect_multi_scale()
    EpCascadeClassifier const *get_data(void) const;
    
    /// Get classifier size in bytes (the data that will be uploaded to a core)
    int get_size(void) const;

private:
    EpCascadeClassifier ep_cascade_classifier;
};

/**
 * Wrapper around corresponding C routine (@see ep_detect_multi_scale).
 * In addition this routine does objects grouping.
 * @param min_neighbors: minimal number of detections in detection group.
 *                       if this value is zero then grouping is disabled.
 */
EpErrorCode detect_multi_scale (
    cv::Mat               const &image,
    CascadeClassifier     const &classifier,
    std::vector<cv::Rect>       &objects,
    int                   const  min_neighbors  = 3,
    EpScanMode            const  scan_mode      = SCAN_EVEN,
    EpDetectionMode       const  detection_mode = DET_HOST,
    int                          num_cores      = 16,
    std::string           const &log_file       = std::string()
);

}

#endif
