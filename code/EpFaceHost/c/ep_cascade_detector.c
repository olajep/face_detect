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
 * Adapteva implementation of LBP face detection algorithm
 * Exported function names start with "ep_"
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <unistd.h>
#include <opencv/cv.h>

#include <omp.h>

#ifndef DEVICE_EMULATION
  //  #include <e_host.h>
    #include <e-loader.h>
    #include <e-hal.h>
    #define DRAM_ADR 0x81000000
    #define BUF_OFFSET 0x01000000
    //#define BUF_OFFSET 0x0
#else//DEVICE_EMULATION
    #include "ep_emulator.h"
    #define DRAM_ADR ((unsigned char*)&(dram_memory.common_memory))
#endif//DEVICE_EMULATION

#define ROWS 4
#define COLS 4

#include "ep_cascade_detector.h"

typedef struct
{
	e_platform_t eplat;
	e_epiphany_t edev;
	e_mem_t emem;
} ep_context_t;


////////////////////////////////////////////////////////////////////////////////
//                             IMAGE FUNCTIONS                                //
////////////////////////////////////////////////////////////////////////////////

/**
 * Divide two positive integers rounding result up
 */
static int divide_up(int const x, int const y) {
    return (x + y - 1) / y;
}

/**
 * Divide two positive integers rounding result to the nearest integer
 */
static int divide_round(int const x, int const y) {
    return (x + y / 2) / y;
}

/**
 * Round number down to the value dividible by 8
 */
static int round_down_to_8n(int const x) {
    return x & (-8);
}

/**
 * Round number up to the value dividible by 8
 */
static int round_up_to_8n(int const x) {
    return (x + 7) & (-8);
}

/**
 * Round number to the nearest value dividible by 8
 */
static int round_to_8n(int const x) {
	return (x + 4) & (-8);
}
/**
 * Create empty EpImage.
 * @return value that is recognized by other functions as "empty"
 */
EpImage ep_image_create_empty(void) {
    EpImage result = {NULL, 0, 0, 0};
    return result;
}

/**
 * Check whether image is empty.
 * @param image: pointer to valid image structure.
 * @return non-zero value for empty image, otherwise zero .
 */
int ep_image_is_empty(EpImage const *const image) {
    return !image->data;
}

/**
 * Create EpImage given its width and height.
 * @param width: required image width;
 * @param height: required image height.
 * @return image with required size and undefined contents,
 *   or empty image on memory allocation failure. 
 */
EpImage ep_image_create(int const width, int const height) {
    int step = round_up_to_8n(width);
    EpImage result = { (unsigned char *)malloc(step * height), width, height, step };
    if( !result.data )
        result.width = result.height = result.step = 0;
    return result;
}

/**
 * Get subimage of specified image without data copying.
 *   No checks on specified coordinates are performed.
 * @param image: pointer to valid image structure;
 * @param x: horizontal coordinate of upper-left corner of required subimage;
 * @param y: vertical coordinate of upper-left corner of required subimage;
 * @param width: width of required subimage;
 * @param height: height of required subimage.
 * @return subimage required.
 */
EpImage ep_subimage_get (
    EpImage const *const image,
    int const x    , int const y    ,
    int const width, int const height
) {
    EpImage result = {image->data + y * image->step + x, width, height, image->step};
    return result;
}

/**
 * Get subimage of specified image with data copying.
 *   No checks on specified coordinates are performed.
 * @param image: pointer to valid image structure;
 * @param x: horizontal coordinate of upper-left corner of required subimage;
 * @param y: vertical coordinate of upper-left corner of required subimage;
 * @param width: width of required subimage;
 * @param height: height of required subimage.
 * @return subimage required; on memory allocation failure empty image will be returned
 */
EpImage ep_subimage_clone (
    EpImage const*const image,
    int const x, int const y,
    int const width, int const height
) {
    EpImage result = ep_image_create(width, height);
    if( ep_image_is_empty(&result) )
        return result;

    int const image_step = image->step;
    unsigned char const *source_pixels = image->data + y * image_step + x;
    unsigned char *result_pixels = result.data;

    if(image_step == result.step) {
        memcpy(result_pixels, source_pixels, width * height);
    } else {
        for(int line = 0; line < height; ++line) {
            memcpy(result_pixels, source_pixels, width);
            result_pixels += result.step;
            source_pixels += image_step;
        }
    }

    return result;
}

/**
 * Copy the whole image and its data.
 * @param image: pointer to valid image structure.
 * @return copy of the image and its data; on memory allocation failure empty image will be returned
 */
EpImage ep_image_clone(EpImage const*const image) {
    return ep_subimage_clone(image, 0, 0, image->width, image->height);
}

/**
 * Calculates image checksum for debugging purpose.
 *   Images with the same data will get the same checksums,
 *   but it is not guaranteed that images with different data will
 *   get diffent checksums.
 * @param image: pointer to valid image structure.
 * @return checksum (int value).
 */
int ep_image_checksum(EpImage const *const image) {
    int result = 0;

    unsigned char const *image_data = image->data;
    int const image_width = image->width, image_height = image->height;
    int const image_step = image->step;

    if(image_step == image_width) {

        unsigned char const *const stop_addr = image_data + image_width * image_height;
        for(; image_data < stop_addr; ++image_data)
            result += *image_data;

    } else {

        for(int y = 0; y < image_height; ++y) {
            unsigned char const *pixel = image_data;
            unsigned char const *const stop_addr = pixel + image_width;
            for(; pixel < stop_addr; ++pixel)
                result += *pixel;
            image_data += image_step;
        }

    }

    return result;
}

/**
 * Save image to simple binary file for debug purpose.
 * @param image: pointer to valid image structure.
 * @param file_name: pointer to file name (null-terminated string)
 * @return ERR_SUCCESS on success;
 *         ERR_ARGUMENT if image is empty (empty images cannot be saved);
 *         ERR_FILE if file cannot be opened or written.
 */
EpErrorCode ep_image_save(EpImage const *const image, char const *const file_name) {
    if( ep_image_is_empty(image) )
        return ERR_ARGUMENT; //Bad image

    if(image->width <= 0 || image->height <= 0)
        return ERR_ARGUMENT; //Bad image

    FILE *const file = fopen(file_name, "wb");
    if( !file )
        return ERR_FILE; //Bad file

    int const id = FILE_ID_IMAGE;
    if(fwrite(&id, sizeof(id), 1, file) != 1) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    unsigned char const *image_data = image->data;
    int const image_width = image->width, image_height = image->height;
    int const image_step = image->step;

    if(fwrite(&image_width, sizeof(image_width), 1, file) != 1) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    if(fwrite(&image_height, sizeof(image_height), 1, file) != 1) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    if(image_step == image_width) {

        int const data_size = image_width * image_height;
        if( (int)fwrite(image_data, 1, data_size, file) != data_size ) {
            fclose(file);
            return ERR_FILE; //Unsuccessful write
        }

    } else {

        for(int y = 0; y < image_height; ++y) {
            if( (int)fwrite(image_data, 1, image_width, file) != image_width ) {
                fclose(file);
                return ERR_FILE; //Unsuccessful write
            }
            image_data += image_step;
        }

    }

    fclose(file);

    return ERR_SUCCESS;
}

/**
 * Load image from simple binary file ( previously written by ep_image_save() )
 * @param file_name: pointer to file name (null-terminated string)
 * @param error_code: pointer to integer value which will receive the error code.
 *                  If this pointer is NULL then no error code is stored.
 *     Error codes: ERR_SUCCESS -- success;
 *                  ERR_FILE -- file cannot be opened or read;
 *                  ERR_FILE_CONTENTS -- wrong file contents;
 *                  ERR_MEMORY -- cannot allocate memory buffer.
 * @return image loaded. Empty image is returned in case of any error.
 */
EpImage ep_image_load(char const *const file_name, EpErrorCode *const error_code) {
    EpImage result = ep_image_create_empty();
    FILE *const file = fopen(file_name, "rb");
    if( !file ) {
        if(error_code) *error_code = ERR_FILE;
        return result; //Bad file
    }

    int id;
    if(fread(&id, sizeof(id), 1, file) != 1 || id != FILE_ID_IMAGE) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        return result; //Bad file contents
    }

    int image_width;
    if(fread(&image_width, sizeof(image_width), 1, file) != 1 || image_width <= 0) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        return result; //Bad file contents
    }

    int image_height;
    if(fread(&image_height, sizeof(image_height), 1, file) != 1 || image_height <= 0) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        return result; //Bad file contents
    }

    result = ep_image_create(image_width, image_height);
    if( ep_image_is_empty(&result) ) {
        if(error_code) *error_code = ERR_MEMORY;
        fclose(file);
        return result; //Cannot allocate memory buffer
    }

    int const data_size = image_width * image_height;
    if( (int)fread(result.data, 1, data_size, file) != data_size ) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        ep_image_release(&result); //Bad file contents
    }

    if(error_code) *error_code = ERR_SUCCESS;
    fclose(file);
    return result;
}

/**
 * Release image. After calling this function image will be empty. 
 * @param image: pointer to valid image structure.
 */
void ep_image_release(EpImage *const image) {
    free(image->data);
    image->data = NULL;
    image->width = 0;
    image->height = 0;
    image->step = 0;
}

////////////////////////////////////////////////////////////////////////////////
//                        IMAGE LIST FUNCTIONS                                //
////////////////////////////////////////////////////////////////////////////////

/**
 * Create empty images list
 */
EpImgList ep_img_list_create_empty(int const start_offset) {
    EpImgList result = {NULL, 0, 0, start_offset, start_offset};
    return result;
}

/**
 * Add item to images list.
 * @param img_list: pointer to valid images list;
 * @return ERR_SUCCESS on success;
 *         ERR_MEMORY on memory allocation failure.
 */
EpErrorCode ep_img_list_add (
    EpImgList *const img_list,
    int        const step,
    int        const width,
    int        const height
) {
    if(img_list->count == img_list->capacity) {
        int const new_capacity = img_list->capacity + 4;
        EpImageProp *const new_buf = (EpImageProp *)realloc(img_list->data, sizeof(EpImageProp) * new_capacity);
        if( !new_buf )
            return ERR_MEMORY; //Failed to reallocate memory buffer
        img_list->data = new_buf;
        img_list->capacity = new_capacity;
    }

    EpImageProp *const new_img = img_list->data + img_list->count;

    new_img->data_offset = img_list->cur_offset;
    new_img->step        = step;
    new_img->width       = width;
    new_img->height      = height;

    img_list->prev_offset = img_list->cur_offset;
    img_list->cur_offset += step * height;

    ++img_list->count;
    return ERR_SUCCESS;
}

/**
 * Release data hold by image list.
 * After calling this function list becomes empty. Images properties can be inserted to such list.
 * @param img_list: pointer to valid image list.
 */
void ep_img_list_release(EpImgList *const img_list) {
    free(img_list->data);
    img_list->data = NULL;
    img_list->capacity = 0;
    img_list->count = 0;
}

////////////////////////////////////////////////////////////////////////////////
//                        RECTANGLES LIST FUNCTIONS                           //
////////////////////////////////////////////////////////////////////////////////

/**
 * Create empty rectangles list
 */
EpRectList ep_rect_list_create_empty(void) {
    EpRectList result = {NULL, 0, 0};
    return result;
}

/**
 * Add item to rectangles list.
 * @param rect_list: pointer to valid rectangles list;
 * @param x: horizontal coordinate of left rectangle side;
 * @param y: vertical coordinate of top rectangle size;
 * @width: width of the rectangle;
 * @height: height of the rectangle.
 * @return ERR_SUCCESS on success;
 *         ERR_MEMORY on memory allocation failure.
 */
EpErrorCode ep_rect_list_add (
    EpRectList *const rect_list,
    float const x,
    float const y,
    float const width,
    float const height
) {
    if(rect_list->count == rect_list->capacity) {
        int const new_capacity = rect_list->capacity + MAX_DETECTIONS_PER_TILE;
        EpRect *const new_buf = (EpRect *)realloc(rect_list->data, sizeof(EpRect) * new_capacity);
        if( !new_buf )
            return ERR_MEMORY; //Failed to reallocate memory buffer
        rect_list->data = new_buf;
        rect_list->capacity = new_capacity;
    }

    EpRect *const new_rect = rect_list->data + rect_list->count;

    new_rect->x     = x    ; new_rect->y      = y     ;
    new_rect->width = width; new_rect->height = height;

    ++rect_list->count;
    return ERR_SUCCESS;
}

/**
 * Reserve some space in rectangles list to speed up future rectangles insertions.
 * @param rect_list: pointer to valid rectangles list;
 * @param count: required number of rectangles that are planned to be added.
 * @return ERR_SUCCESS on success;
 *         ERR_MEMORY on memory allocation failure; list is not changed in this case.
 */
EpErrorCode ep_rect_list_reserve(EpRectList *const rect_list, int const count) {
    if(rect_list->count + count <= rect_list->capacity)
        return ERR_SUCCESS;

    int const new_capacity = rect_list->count + count + MAX_DETECTIONS_PER_TILE;
    EpRect *const new_buf = (EpRect *)realloc(rect_list->data, sizeof(EpRect) * new_capacity);
    if( !new_buf )
        return ERR_MEMORY; //Failed to reallocate memory buffer

    rect_list->data = new_buf;
    rect_list->capacity = new_capacity;

    return ERR_SUCCESS;
}

/**
 * Release data hold by rectangles list.
 * After calling this function list becomes empty. Rectangles can be inserted to such list.
 * @param rect_list: pointer to valid rectangles list.
 */
void ep_rect_list_release(EpRectList *const rectList) {
    free(rectList->data);
    rectList->data = NULL;
    rectList->capacity = 0;
    rectList->count = 0;
}

////////////////////////////////////////////////////////////////////////////////
//                        TASK LIST FUNCTIONS                                 //
////////////////////////////////////////////////////////////////////////////////

/**
 * Create empty task list
 */
EpTaskList ep_task_list_create_empty(void) {
    EpTaskList result = {NULL, 0, 0};
    return result;
}

/**
 * Add item to tasks list.
 * @param task_list: pointer to valid rectangles list;
 * @param area       : tile area (must be  width * step)
 * @param width      : width of tile
 * @param height     : height of tile
 * @param step       : step in tile (must be round_up_to8(width))
 * @param scan_mode  : Scan mode of pixels (even pixels, odd pixels, or all pixels)
 * @param items_count: count of detected items (must be 0)
 * @param image_index: index of processing image
 * @return ERR_SUCCESS on success;
 *         ERR_MEMORY on memory allocation failure.
 */
EpErrorCode ep_task_list_add (
    EpTaskList *const task_list,
    int offset,
    int width,
    int height,
    int step,
    int scan_mode,
    int items_count,
    int image_index
) {
    if(task_list->count == task_list->capacity) {
        int const new_capacity = task_list->capacity + MAX_DETECTIONS_PER_TILE;
        EpTaskItem *const new_buf = (EpTaskItem *)realloc(task_list->data, sizeof(EpTaskItem) * new_capacity);
        if( !new_buf )
            return ERR_MEMORY; //Failed to reallocate memory buffer
        task_list->data = new_buf;
        task_list->capacity = new_capacity;
    }

    EpTaskItem *const new_task = task_list->data + task_list->count;

    new_task->offset      = offset;
    new_task->area        = step * height;
    new_task->width       = width;
    new_task->height      = height;
    new_task->step        = step;
    new_task->scan_mode   = scan_mode;
    new_task->items_count = items_count;
    new_task->image_index = image_index;

    ++task_list->count;
    return ERR_SUCCESS;
}

/**
 * Reserve some space in task list to speed up future tasks insertions.
 * @param task_list: pointer to valid task list;
 * @param count: required number of tasks that are planned to be added.
 * @return ERR_SUCCESS on success;
 *         ERR_MEMORY on memory allocation failure; list is not changed in this case.
 */
EpErrorCode ep_task_list_reserve(EpTaskList *const task_list, int const count) {
    if(task_list->count + count <= task_list->capacity)
        return ERR_SUCCESS;

    int const new_capacity = task_list->count + count + MAX_DETECTIONS_PER_TILE;
    EpTaskItem *const new_buf = (EpTaskItem *)realloc(task_list->data, sizeof(EpTaskItem) * new_capacity);
    if( !new_buf )
        return ERR_MEMORY; //Failed to reallocate memory buffer

    task_list->data = new_buf;
    task_list->capacity = new_capacity;

    return ERR_SUCCESS;
}

/**
 * Release data hold by task list.
 * After calling this function list becomes empty. Rectangles can be inserted to such list.
 * @param task_list: pointer to valid task list.
 */
void ep_task_list_release(EpTaskList *const task_list) {
    free(task_list->data);
    task_list->data = NULL;
    task_list->capacity = 0;
    task_list->count = 0;
}

////////////////////////////////////////////////////////////////////////////////
//                          CLASSIFIER FUNCTIONS                              //
////////////////////////////////////////////////////////////////////////////////

/**
 * Create classifier which is recognised by other functions as "empty".
 * @return required classifier structure.
 */
EpCascadeClassifier ep_classifier_create_empty(void) {
    EpCascadeClassifier result = {NULL, 0};
    return result;
}

/**
 * Check whether classifier is empty.
 * @param pointer to the valid classifier structure.
 * @return non-zero value for empty classifier; zero for non-empty.
 */
int ep_classifier_is_empty(EpCascadeClassifier const *const classifier) {
    return !classifier->data;
}

/**
 * Check classifier data for validity. Empty classifier is considered invalid!
 * Use ep_classifier_is_empty() function to check whether classifier is empty.
 * @param classifier: pointer to tested classifier. classifier->data must be safely dereferencable!
 * @return zero value for good classifier data; non-zero value for bad data.
 */
int ep_classifier_check(EpCascadeClassifier const *const classifier) {
    if( ep_classifier_is_empty(classifier) )
        return 1; //Empty classifier

    //ToDo: implement full checking here

    int const size = classifier->size;

    if( size < (int)( sizeof(EpNodeMeta) + sizeof(EpNodeDecision) +
        sizeof(EpNodeStage) + sizeof(EpNodeFinal) ) )
        return 2; //Classifier is too small

    char const *const first_item = classifier->data;

    if(*(int const *)first_item != NODE_META)
        return 3; //No meta node at the beginning

    if( ((EpNodeMeta const *)first_item)->window_height < 3 ||
        ((EpNodeMeta const *)first_item)->window_width  < 3 )
        return 4; //Window size is too small

    char const *const second_item = first_item + sizeof(EpNodeMeta);
    if(*(int const *)second_item != NODE_DECISION)
        return 5; //Second node must be EpNodeDecision

    char const *const last_item = first_item + size - sizeof(EpNodeFinal);
    if(*(int const *)last_item != NODE_FINAL)
        return 6; //Last node must be EpNodeFinal

    char const *const before_last_item = last_item - sizeof(EpNodeStage);
    if(*(int const *)before_last_item != NODE_STAGE)
        return 7; //Node before the last one must be EpNodeStage

    return 0;
}

/**
 * Clone classifier data into new buffer.
 * @param classifier: pointer to the classifier to be cloned.
 * @return cloned classifier structure; empty classifier is returned in case of any error.
 */
EpCascadeClassifier ep_classifier_clone(EpCascadeClassifier const *const classifier) {
    if( ep_classifier_is_empty(classifier) )
        return ep_classifier_create_empty();

    int const classifier_size = classifier->size;

    EpCascadeClassifier const result =
        { (char *)malloc(classifier_size), classifier_size };

    if(!result.data)
        return ep_classifier_create_empty();

    memcpy(result.data, classifier->data, classifier_size);

    return result;
}

/**
 * Calculate classifier checksum for debug purpose.
 *   Classifiers with the same data will get the same checksums,
 *   but it is not guaranteed that classifiers with different data will
 *   get different checksums.
 * @param classifier: pointer to valid classifier structure.
 * @return checksum (int value).
 */
int ep_classifier_checksum(EpCascadeClassifier const *const classifier) {
    int const classifier_size = classifier->size;
    char const *const classifier_data = classifier->data;
    int result = 0;
    for(int i = 0; i < classifier_size; ++i)
        result += classifier_data[i];
    return result;
}

/**
 * Save classifier to binary file.
 * @param classifier: pointer to valid classifier structure.
 * @param file_name: pointer to file name (null-terminated string).
 * @return ERR_SUCCESS on success;
 *         ERR_ARGUMENT if classifier is empty (empty classifiers cannot be saved);
 *         ERR_FILE if file cannot be opened or written.
 */
EpErrorCode ep_classifier_save (
    EpCascadeClassifier const *const classifier,
    char const *const file_name
) {
    if( ep_classifier_is_empty(classifier) )
        return ERR_ARGUMENT; //Empty classifier

    int const classifier_size = classifier->size;

    if(classifier_size <= 0)
        return ERR_ARGUMENT; //Bad classifier

    FILE *const file = fopen(file_name, "wb");
    if( !file )
        return ERR_FILE; //Bad file

    int const id = FILE_ID_CLASSIFIER;
    if(fwrite(&id, sizeof(id), 1, file) != 1) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    if(fwrite(&classifier_size, sizeof(classifier_size), 1, file) != 1) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    if( (int)fwrite(classifier->data, 1, classifier_size, file) != classifier_size ) {
        fclose(file);
        return ERR_FILE; //Unsuccessful write
    }

    fclose(file);

    return ERR_SUCCESS;
}

/**
 * Load classifier from binary file ( previously written by ep_classifier_save() )
 * @param file_name: pointer to file name (null-terminated string)
 * @param error_code: pointer to integer value which will receive the error code.
 *                  If this pointer is NULL then no error code is stored.
 *     Error codes: ERR_SUCCESS -- success;
 *                  ERR_FILE -- file cannot be opened or read;
 *                  ERR_FILE_CONTENTS -- wrong file contents;
 *                  ERR_MEMORY -- cannot allocate memory buffer.
 * @return classifier loaded. Empty classifier is returned in case of any error.
 */
EpCascadeClassifier ep_classifier_load(char const *const file_name, EpErrorCode *const error_code) {
    EpCascadeClassifier result = ep_classifier_create_empty();

    FILE *const file = fopen(file_name, "rb");
    if( !file ) {
        if(error_code) *error_code = ERR_FILE;
        return result; //Bad file
    }

    int id;
    if(fread(&id, sizeof(id), 1, file) != 1 || id != FILE_ID_CLASSIFIER) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        return result;
    }

    int classifier_size;
    if(fread(&classifier_size, sizeof(classifier_size), 1, file) != 1 || classifier_size <= 0) {
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        return result;
    }

    result.size = classifier_size;
    result.data = (char *)malloc(classifier_size);

    if( !result.data ) { //Cannot allocate memory buffer
        if(error_code) *error_code = ERR_MEMORY;
        fclose(file);
        result.size = 0;
        return result;
    }

    if( (int)fread(result.data, 1, classifier_size, file) != classifier_size ) { //Can not read file contents
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        fclose(file);
        ep_classifier_release(&result);
        return result;
    }

    fclose(file);

    if( ep_classifier_check(&result) ) { //Wrong data read
        if(error_code) *error_code = ERR_FILE_CONTENTS;
        ep_classifier_release(&result);
        return result;
    }

    if(error_code) *error_code = ERR_SUCCESS;
    return result;
}

/**
 * Release memory hold by classifier.
 * After calling this function classifier is empty.
 * @param classifier: pointer to valid classifier structure. 
 */
void ep_classifier_release(EpCascadeClassifier *const classifier) {
    free(classifier->data);
    classifier->data = NULL;
    classifier->size = 0;
}

////////////////////////////////////////////////////////////////////////////////
//                            DETECTION FUNCTIONS                             //
////////////////////////////////////////////////////////////////////////////////

/**
 * Scale image which size is 8x into images which sizes are 7x, 6x, 5x and 4x
 *   One of resulting images can occupy the same memory as source image.
 *   In this case their steps must be equal. All memory must be preallocated
 *
 * @param src8: Source image.
 * If sizes are not multiples of 8 then some pixels near borders will be thrown away
 * @param out7: Resulting image.
 * @param out6: Resulting image.
 * @param out5: Resulting image.
 * @param offs_x: pointer to integer variable to store number of pixels thrown away from left side
 * @param offs_y: pointer to integer variable to store number of pixels thrown away from top side
 */
///
static void scale8765 (
    EpImage const *const src8,
    EpImage *const out7,
    EpImage *const out6,
    EpImage *const out5,
    int *const offs_x,
    int *const offs_y
) {
    int const src_width = src8->width, src_height = src8->height;
    int const blocks_width = src_width / 8, blocks_height = src_height / 8;
    int const offset_x = (src_width % 8) / 2, offset_y = (src_height % 8) / 2;

    if(offs_x) *offs_x = offset_x;
    if(offs_y) *offs_y = offset_y;

    out7->width = blocks_width * 7; out7->height = blocks_height * 7;
    out6->width = blocks_width * 6; out6->height = blocks_height * 6;
    out5->width = blocks_width * 5; out5->height = blocks_height * 5;

    // Auto generated code below
    for(int block_y = 0; block_y < blocks_height; ++block_y) {
        int const y8 = block_y * 8 + offset_y,
                  y7 = block_y * 7,
                  y6 = block_y * 6,
                  y5 = block_y * 5;
        //ToDo: remove multiplications from scan lines calculation

        unsigned char const *const s8_row0 = src8->data + src8->step * (y8    ),
                            *const s8_row1 = src8->data + src8->step * (y8 + 1),
                            *const s8_row2 = src8->data + src8->step * (y8 + 2),
                            *const s8_row3 = src8->data + src8->step * (y8 + 3),
                            *const s8_row4 = src8->data + src8->step * (y8 + 4),
                            *const s8_row5 = src8->data + src8->step * (y8 + 5),
                            *const s8_row6 = src8->data + src8->step * (y8 + 6),
                            *const s8_row7 = src8->data + src8->step * (y8 + 7);

        unsigned char *const o7_row0 = out7->data + out7->step * (y7    ),
                      *const o7_row1 = out7->data + out7->step * (y7 + 1),
                      *const o7_row2 = out7->data + out7->step * (y7 + 2),
                      *const o7_row3 = out7->data + out7->step * (y7 + 3),
                      *const o7_row4 = out7->data + out7->step * (y7 + 4),
                      *const o7_row5 = out7->data + out7->step * (y7 + 5),
                      *const o7_row6 = out7->data + out7->step * (y7 + 6);

        unsigned char *const o6_row0 = out6->data + out6->step * (y6    ),
                      *const o6_row1 = out6->data + out6->step * (y6 + 1),
                      *const o6_row2 = out6->data + out6->step * (y6 + 2),
                      *const o6_row3 = out6->data + out6->step * (y6 + 3),
                      *const o6_row4 = out6->data + out6->step * (y6 + 4),
                      *const o6_row5 = out6->data + out6->step * (y6 + 5);

        unsigned char *const o5_row0 = out5->data + out5->step * (y5    ),
                      *const o5_row1 = out5->data + out5->step * (y5 + 1),
                      *const o5_row2 = out5->data + out5->step * (y5 + 2),
                      *const o5_row3 = out5->data + out5->step * (y5 + 3),
                      *const o5_row4 = out5->data + out5->step * (y5 + 4);

        for(int block_x = 0; block_x < blocks_width; ++block_x) {
            int const x8 = block_x * 8 + offset_x,
                      x7 = block_x * 7,
                      x6 = block_x * 6,
                      x5 = block_x * 5;

            int const s8p00=s8_row0[x8], s8p01=s8_row0[x8+1], s8p02=s8_row0[x8+2], s8p03=s8_row0[x8+3], s8p04=s8_row0[x8+4], s8p05=s8_row0[x8+5], s8p06=s8_row0[x8+6], s8p07=s8_row0[x8+7],
                      s8p10=s8_row1[x8], s8p11=s8_row1[x8+1], s8p12=s8_row1[x8+2], s8p13=s8_row1[x8+3], s8p14=s8_row1[x8+4], s8p15=s8_row1[x8+5], s8p16=s8_row1[x8+6], s8p17=s8_row1[x8+7],
                      s8p20=s8_row2[x8], s8p21=s8_row2[x8+1], s8p22=s8_row2[x8+2], s8p23=s8_row2[x8+3], s8p24=s8_row2[x8+4], s8p25=s8_row2[x8+5], s8p26=s8_row2[x8+6], s8p27=s8_row2[x8+7],
                      s8p30=s8_row3[x8], s8p31=s8_row3[x8+1], s8p32=s8_row3[x8+2], s8p33=s8_row3[x8+3], s8p34=s8_row3[x8+4], s8p35=s8_row3[x8+5], s8p36=s8_row3[x8+6], s8p37=s8_row3[x8+7],
                      s8p40=s8_row4[x8], s8p41=s8_row4[x8+1], s8p42=s8_row4[x8+2], s8p43=s8_row4[x8+3], s8p44=s8_row4[x8+4], s8p45=s8_row4[x8+5], s8p46=s8_row4[x8+6], s8p47=s8_row4[x8+7],
                      s8p50=s8_row5[x8], s8p51=s8_row5[x8+1], s8p52=s8_row5[x8+2], s8p53=s8_row5[x8+3], s8p54=s8_row5[x8+4], s8p55=s8_row5[x8+5], s8p56=s8_row5[x8+6], s8p57=s8_row5[x8+7],
                      s8p60=s8_row6[x8], s8p61=s8_row6[x8+1], s8p62=s8_row6[x8+2], s8p63=s8_row6[x8+3], s8p64=s8_row6[x8+4], s8p65=s8_row6[x8+5], s8p66=s8_row6[x8+6], s8p67=s8_row6[x8+7],
                      s8p70=s8_row7[x8], s8p71=s8_row7[x8+1], s8p72=s8_row7[x8+2], s8p73=s8_row7[x8+3], s8p74=s8_row7[x8+4], s8p75=s8_row7[x8+5], s8p76=s8_row7[x8+6], s8p77=s8_row7[x8+7];

            //ToDo: improve all these coefficients
            o7_row0[x7    ] = (s8p00 * 49 + s8p01 * 7 + s8p10 * 7 + s8p11 + 32) / 64;
            o7_row0[x7 + 1] = (s8p01 * 42 + s8p02 * 14 + s8p11 * 6 + s8p12 * 2 + 32) / 64;
            o7_row0[x7 + 2] = (s8p02 * 35 + s8p03 * 21 + s8p12 * 5 + s8p13 * 3 + 32) / 64;
            o7_row0[x7 + 3] = (s8p03 * 28 + s8p04 * 28 + s8p13 * 4 + s8p14 * 4 + 32) / 64;
            o7_row0[x7 + 4] = (s8p04 * 21 + s8p05 * 35 + s8p14 * 3 + s8p15 * 5 + 32) / 64;
            o7_row0[x7 + 5] = (s8p05 * 14 + s8p06 * 42 + s8p15 * 2 + s8p16 * 6 + 32) / 64;
            o7_row0[x7 + 6] = (s8p06 * 7 + s8p07 * 49 + s8p16 + s8p17 * 7 + 32) / 64;
            o7_row1[x7    ] = (s8p10 * 42 + s8p11 * 6 + s8p20 * 14 + s8p21 * 2 + 32) / 64;
            o7_row1[x7 + 1] = (s8p11 * 36 + s8p12 * 12 + s8p21 * 12 + s8p22 * 4 + 32) / 64;
            o7_row1[x7 + 2] = (s8p12 * 30 + s8p13 * 18 + s8p22 * 10 + s8p23 * 6 + 32) / 64;
            o7_row1[x7 + 3] = (s8p13 * 24 + s8p14 * 24 + s8p23 * 8 + s8p24 * 8 + 32) / 64;
            o7_row1[x7 + 4] = (s8p14 * 18 + s8p15 * 30 + s8p24 * 6 + s8p25 * 10 + 32) / 64;
            o7_row1[x7 + 5] = (s8p15 * 12 + s8p16 * 36 + s8p25 * 4 + s8p26 * 12 + 32) / 64;
            o7_row1[x7 + 6] = (s8p16 * 6 + s8p17 * 42 + s8p26 * 2 + s8p27 * 14 + 32) / 64;
            o7_row2[x7    ] = (s8p20 * 35 + s8p21 * 5 + s8p30 * 21 + s8p31 * 3 + 32) / 64;
            o7_row2[x7 + 1] = (s8p21 * 30 + s8p22 * 10 + s8p31 * 18 + s8p32 * 6 + 32) / 64;
            o7_row2[x7 + 2] = (s8p22 * 25 + s8p23 * 15 + s8p32 * 15 + s8p33 * 9 + 32) / 64;
            o7_row2[x7 + 3] = (s8p23 * 20 + s8p24 * 20 + s8p33 * 12 + s8p34 * 12 + 32) / 64;
            o7_row2[x7 + 4] = (s8p24 * 15 + s8p25 * 25 + s8p34 * 9 + s8p35 * 15 + 32) / 64;
            o7_row2[x7 + 5] = (s8p25 * 10 + s8p26 * 30 + s8p35 * 6 + s8p36 * 18 + 32) / 64;
            o7_row2[x7 + 6] = (s8p26 * 5 + s8p27 * 35 + s8p36 * 3 + s8p37 * 21 + 32) / 64;
            o7_row3[x7    ] = (s8p30 * 28 + s8p31 * 4 + s8p40 * 28 + s8p41 * 4 + 32) / 64;
            o7_row3[x7 + 1] = (s8p31 * 24 + s8p32 * 8 + s8p41 * 24 + s8p42 * 8 + 32) / 64;
            o7_row3[x7 + 2] = (s8p32 * 20 + s8p33 * 12 + s8p42 * 20 + s8p43 * 12 + 32) / 64;
            o7_row3[x7 + 3] = (s8p33 * 16 + s8p34 * 16 + s8p43 * 16 + s8p44 * 16 + 32) / 64;
            o7_row3[x7 + 4] = (s8p34 * 12 + s8p35 * 20 + s8p44 * 12 + s8p45 * 20 + 32) / 64;
            o7_row3[x7 + 5] = (s8p35 * 8 + s8p36 * 24 + s8p45 * 8 + s8p46 * 24 + 32) / 64;
            o7_row3[x7 + 6] = (s8p36 * 4 + s8p37 * 28 + s8p46 * 4 + s8p47 * 28 + 32) / 64;
            o7_row4[x7    ] = (s8p40 * 21 + s8p41 * 3 + s8p50 * 35 + s8p51 * 5 + 32) / 64;
            o7_row4[x7 + 1] = (s8p41 * 18 + s8p42 * 6 + s8p51 * 30 + s8p52 * 10 + 32) / 64;
            o7_row4[x7 + 2] = (s8p42 * 15 + s8p43 * 9 + s8p52 * 25 + s8p53 * 15 + 32) / 64;
            o7_row4[x7 + 3] = (s8p43 * 12 + s8p44 * 12 + s8p53 * 20 + s8p54 * 20 + 32) / 64;
            o7_row4[x7 + 4] = (s8p44 * 9 + s8p45 * 15 + s8p54 * 15 + s8p55 * 25 + 32) / 64;
            o7_row4[x7 + 5] = (s8p45 * 6 + s8p46 * 18 + s8p55 * 10 + s8p56 * 30 + 32) / 64;
            o7_row4[x7 + 6] = (s8p46 * 3 + s8p47 * 21 + s8p56 * 5 + s8p57 * 35 + 32) / 64;
            o7_row5[x7    ] = (s8p50 * 14 + s8p51 * 2 + s8p60 * 42 + s8p61 * 6 + 32) / 64;
            o7_row5[x7 + 1] = (s8p51 * 12 + s8p52 * 4 + s8p61 * 36 + s8p62 * 12 + 32) / 64;
            o7_row5[x7 + 2] = (s8p52 * 10 + s8p53 * 6 + s8p62 * 30 + s8p63 * 18 + 32) / 64;
            o7_row5[x7 + 3] = (s8p53 * 8 + s8p54 * 8 + s8p63 * 24 + s8p64 * 24 + 32) / 64;
            o7_row5[x7 + 4] = (s8p54 * 6 + s8p55 * 10 + s8p64 * 18 + s8p65 * 30 + 32) / 64;
            o7_row5[x7 + 5] = (s8p55 * 4 + s8p56 * 12 + s8p65 * 12 + s8p66 * 36 + 32) / 64;
            o7_row5[x7 + 6] = (s8p56 * 2 + s8p57 * 14 + s8p66 * 6 + s8p67 * 42 + 32) / 64;
            o7_row6[x7    ] = (s8p60 * 7 + s8p61 + s8p70 * 49 + s8p71 * 7 + 32) / 64;
            o7_row6[x7 + 1] = (s8p61 * 6 + s8p62 * 2 + s8p71 * 42 + s8p72 * 14 + 32) / 64;
            o7_row6[x7 + 2] = (s8p62 * 5 + s8p63 * 3 + s8p72 * 35 + s8p73 * 21 + 32) / 64;
            o7_row6[x7 + 3] = (s8p63 * 4 + s8p64 * 4 + s8p73 * 28 + s8p74 * 28 + 32) / 64;
            o7_row6[x7 + 4] = (s8p64 * 3 + s8p65 * 5 + s8p74 * 21 + s8p75 * 35 + 32) / 64;
            o7_row6[x7 + 5] = (s8p65 * 2 + s8p66 * 6 + s8p75 * 14 + s8p76 * 42 + 32) / 64;
            o7_row6[x7 + 6] = (s8p66 + s8p67 * 7 + s8p76 * 7 + s8p77 * 49 + 32) / 64;

            o6_row0[x6    ] = (s8p00 * 9 + s8p01 * 3 + s8p10 * 3 + s8p11 + 8) / 16;
            o6_row0[x6 + 1] = (s8p01 * 6 + s8p02 * 6 + s8p11 * 2 + s8p12 * 2 + 8) / 16;
            o6_row0[x6 + 2] = (s8p02 * 3 + s8p03 * 9 + s8p12 + s8p13 * 3 + 8) / 16;
            o6_row0[x6 + 3] = (s8p04 * 9 + s8p05 * 3 + s8p14 * 3 + s8p15 + 8) / 16;
            o6_row0[x6 + 4] = (s8p05 * 6 + s8p06 * 6 + s8p15 * 2 + s8p16 * 2 + 8) / 16;
            o6_row0[x6 + 5] = (s8p06 * 3 + s8p07 * 9 + s8p16 + s8p17 * 3 + 8) / 16;
            o6_row1[x6    ] = (s8p10 * 6 + s8p11 * 2 + s8p20 * 6 + s8p21 * 2 + 8) / 16;
            o6_row1[x6 + 1] = (s8p11 * 4 + s8p12 * 4 + s8p21 * 4 + s8p22 * 4 + 8) / 16;
            o6_row1[x6 + 2] = (s8p12 * 2 + s8p13 * 6 + s8p22 * 2 + s8p23 * 6 + 8) / 16;
            o6_row1[x6 + 3] = (s8p14 * 6 + s8p15 * 2 + s8p24 * 6 + s8p25 * 2 + 8) / 16;
            o6_row1[x6 + 4] = (s8p15 * 4 + s8p16 * 4 + s8p25 * 4 + s8p26 * 4 + 8) / 16;
            o6_row1[x6 + 5] = (s8p16 * 2 + s8p17 * 6 + s8p26 * 2 + s8p27 * 6 + 8) / 16;
            o6_row2[x6    ] = (s8p20 * 3 + s8p21 + s8p30 * 9 + s8p31 * 3 + 8) / 16;
            o6_row2[x6 + 1] = (s8p21 * 2 + s8p22 * 2 + s8p31 * 6 + s8p32 * 6 + 8) / 16;
            o6_row2[x6 + 2] = (s8p22 + s8p23 * 3 + s8p32 * 3 + s8p33 * 9 + 8) / 16;
            o6_row2[x6 + 3] = (s8p24 * 3 + s8p25 + s8p34 * 9 + s8p35 * 3 + 8) / 16;
            o6_row2[x6 + 4] = (s8p25 * 2 + s8p26 * 2 + s8p35 * 6 + s8p36 * 6 + 8) / 16;
            o6_row2[x6 + 5] = (s8p26 + s8p27 * 3 + s8p36 * 3 + s8p37 * 9 + 8) / 16;
            o6_row3[x6    ] = (s8p40 * 9 + s8p41 * 3 + s8p50 * 3 + s8p51 + 8) / 16;
            o6_row3[x6 + 1] = (s8p41 * 6 + s8p42 * 6 + s8p51 * 2 + s8p52 * 2 + 8) / 16;
            o6_row3[x6 + 2] = (s8p42 * 3 + s8p43 * 9 + s8p52 + s8p53 * 3 + 8) / 16;
            o6_row3[x6 + 3] = (s8p44 * 9 + s8p45 * 3 + s8p54 * 3 + s8p55 + 8) / 16;
            o6_row3[x6 + 4] = (s8p45 * 6 + s8p46 * 6 + s8p55 * 2 + s8p56 * 2 + 8) / 16;
            o6_row3[x6 + 5] = (s8p46 * 3 + s8p47 * 9 + s8p56 + s8p57 * 3 + 8) / 16;
            o6_row4[x6    ] = (s8p50 * 6 + s8p51 * 2 + s8p60 * 6 + s8p61 * 2 + 8) / 16;
            o6_row4[x6 + 1] = (s8p51 * 4 + s8p52 * 4 + s8p61 * 4 + s8p62 * 4 + 8) / 16;
            o6_row4[x6 + 2] = (s8p52 * 2 + s8p53 * 6 + s8p62 * 2 + s8p63 * 6 + 8) / 16;
            o6_row4[x6 + 3] = (s8p54 * 6 + s8p55 * 2 + s8p64 * 6 + s8p65 * 2 + 8) / 16;
            o6_row4[x6 + 4] = (s8p55 * 4 + s8p56 * 4 + s8p65 * 4 + s8p66 * 4 + 8) / 16;
            o6_row4[x6 + 5] = (s8p56 * 2 + s8p57 * 6 + s8p66 * 2 + s8p67 * 6 + 8) / 16;
            o6_row5[x6    ] = (s8p60 * 3 + s8p61 + s8p70 * 9 + s8p71 * 3 + 8) / 16;
            o6_row5[x6 + 1] = (s8p61 * 2 + s8p62 * 2 + s8p71 * 6 + s8p72 * 6 + 8) / 16;
            o6_row5[x6 + 2] = (s8p62 + s8p63 * 3 + s8p72 * 3 + s8p73 * 9 + 8) / 16;
            o6_row5[x6 + 3] = (s8p64 * 3 + s8p65 + s8p74 * 9 + s8p75 * 3 + 8) / 16;
            o6_row5[x6 + 4] = (s8p65 * 2 + s8p66 * 2 + s8p75 * 6 + s8p76 * 6 + 8) / 16;
            o6_row5[x6 + 5] = (s8p66 + s8p67 * 3 + s8p76 * 3 + s8p77 * 9 + 8) / 16;

            o5_row0[x5    ] = (s8p00 * 25 + s8p01 * 15 + s8p10 * 15 + s8p11 * 9 + 32) / 64;
            o5_row0[x5 + 1] = (s8p01 * 10 + s8p02 * 25 + s8p03 * 5 + s8p11 * 6 + s8p12 * 15 + s8p13 * 3 + 32) / 64;
            o5_row0[x5 + 2] = (s8p03 * 20 + s8p04 * 20 + s8p13 * 12 + s8p14 * 12 + 32) / 64;
            o5_row0[x5 + 3] = (s8p04 * 5 + s8p05 * 25 + s8p06 * 10 + s8p14 * 3 + s8p15 * 15 + s8p16 * 6 + 32) / 64;
            o5_row0[x5 + 4] = (s8p06 * 15 + s8p07 * 25 + s8p16 * 9 + s8p17 * 15 + 32) / 64;
            o5_row1[x5    ] = (s8p10 * 10 + s8p11 * 6 + s8p20 * 25 + s8p21 * 15 + s8p30 * 5 + s8p31 * 3 + 32) / 64;
            o5_row1[x5 + 1] = (s8p11 * 4 + s8p12 * 10 + s8p13 * 2 + s8p21 * 10 + s8p22 * 25 + s8p23 * 5 + s8p31 * 2 + s8p32 * 5 + s8p33 + 32) / 64;
            o5_row1[x5 + 2] = (s8p13 * 8 + s8p14 * 8 + s8p23 * 20 + s8p24 * 20 + s8p33 * 4 + s8p34 * 4 + 32) / 64;
            o5_row1[x5 + 3] = (s8p14 * 2 + s8p15 * 10 + s8p16 * 4 + s8p24 * 5 + s8p25 * 25 + s8p26 * 10 + s8p34 + s8p35 * 5 + s8p36 * 2 + 32) / 64;
            o5_row1[x5 + 4] = (s8p16 * 6 + s8p17 * 10 + s8p26 * 15 + s8p27 * 25 + s8p36 * 3 + s8p37 * 5 + 32) / 64;
            o5_row2[x5    ] = (s8p30 * 20 + s8p31 * 12 + s8p40 * 20 + s8p41 * 12 + 32) / 64;
            o5_row2[x5 + 1] = (s8p31 * 8 + s8p32 * 20 + s8p33 * 4 + s8p41 * 8 + s8p42 * 20 + s8p43 * 4 + 32) / 64;
            o5_row2[x5 + 2] = (s8p33 * 16 + s8p34 * 16 + s8p43 * 16 + s8p44 * 16 + 32) / 64;
            o5_row2[x5 + 3] = (s8p34 * 4 + s8p35 * 20 + s8p36 * 8 + s8p44 * 4 + s8p45 * 20 + s8p46 * 8 + 32) / 64;
            o5_row2[x5 + 4] = (s8p36 * 12 + s8p37 * 20 + s8p46 * 12 + s8p47 * 20 + 32) / 64;
            o5_row3[x5    ] = (s8p40 * 5 + s8p41 * 3 + s8p50 * 25 + s8p51 * 15 + s8p60 * 10 + s8p61 * 6 + 32) / 64;
            o5_row3[x5 + 1] = (s8p41 * 2 + s8p42 * 5 + s8p43 + s8p51 * 10 + s8p52 * 25 + s8p53 * 5 + s8p61 * 4 + s8p62 * 10 + s8p63 * 2 + 32) / 64;
            o5_row3[x5 + 2] = (s8p43 * 4 + s8p44 * 4 + s8p53 * 20 + s8p54 * 20 + s8p63 * 8 + s8p64 * 8 + 32) / 64;
            o5_row3[x5 + 3] = (s8p44 + s8p45 * 5 + s8p46 * 2 + s8p54 * 5 + s8p55 * 25 + s8p56 * 10 + s8p64 * 2 + s8p65 * 10 + s8p66 * 4 + 32) / 64;
            o5_row3[x5 + 4] = (s8p46 * 3 + s8p47 * 5 + s8p56 * 15 + s8p57 * 25 + s8p66 * 6 + s8p67 * 10 + 32) / 64;
            o5_row4[x5    ] = (s8p60 * 15 + s8p61 * 9 + s8p70 * 25 + s8p71 * 15 + 32) / 64;
            o5_row4[x5 + 1] = (s8p61 * 6 + s8p62 * 15 + s8p63 * 3 + s8p71 * 10 + s8p72 * 25 + s8p73 * 5 + 32) / 64;
            o5_row4[x5 + 2] = (s8p63 * 12 + s8p64 * 12 + s8p73 * 20 + s8p74 * 20 + 32) / 64;
            o5_row4[x5 + 3] = (s8p64 * 3 + s8p65 * 15 + s8p66 * 6 + s8p74 * 5 + s8p75 * 25 + s8p76 * 10 + 32) / 64;
            o5_row4[x5 + 4] = (s8p66 * 9 + s8p67 * 15 + s8p76 * 15 + s8p77 * 25 + 32) / 64;
        }
    }
}

/**
 * Reduce image twice.
 * Resulting image can occupy the same memory as source image. In this case they must have the same step.
 * @param src: pointer to source image;
 * @param out: pointer to resulting image. Memory for resulting image must be preallocated.
 */
static void scale21(EpImage const *const src, EpImage *const out) {
    int const out_width = src->width / 2,
             out_height = src->height / 2;

    out->width = out_width;
    out->height = out_height;

    //ToDo: remove multiplications from scanlines calculation

    for(int y = 0; y < out_height; ++y) {
        unsigned char const *const sls1 = src->data + src->step * y * 2;
        unsigned char const *const sls2 = sls1 + src->step;
        unsigned char       *const slo  = out->data + out->step * y;

        for(int x = 0; x < out_width; ++x) {
            int const x2 = x << 1;
            slo[x] = (sls1[x2] + sls1[x2 + 1] +
                      sls2[x2] + sls2[x2 + 1] + 2) >> 2;
        }
    }
}

/**
 * Scale image inplace with realloc in memory;
 * @param src: pointer to source image;
 */
static void scale21_realloc(EpImage *const src){
    EpImage temp = ep_image_create(src->width / 2, src->height / 2);
    scale21(src, &temp);
    ep_image_release(src);
    *src = temp;
}

/**
 * Calculate decision based on value of LBP feature
 *
 * @param image_data: Position in memory where to sample feature from.
 * @param image_step: Step in bytes from one image line to the next image line.
 * @param node: Classifier node used to make decision.
 * @return decision value: 0 or 1.
 */
static int calc_lbp_decision (
    unsigned char const *image_data,
    int const image_step,
    EpNodeDecision const *const node
) {
    int const feature = node->feature;

    //Shifting position according to LBP feature position
    image_data += ( (feature >> 16) & 255 ) + (feature >> 24) * image_step;

    int sum00, sum01, sum02,
        sum10, sum11, sum12,
        sum20, sum21, sum22;

    int const feature_width =  feature       & 255,
             feature_height = (feature >> 8) & 255;

    if(feature_width == 1) {

        if(feature_height == 1) { //Optimization for block 1 by 1 pixel

            unsigned char const *const sl0 = image_data,
                                *const sl1 = sl0 + image_step,
                                *const sl2 = sl1 + image_step;

            sum00 = sl0[0]; sum01 = sl0[1]; sum02 = sl0[2];
            sum10 = sl1[0]; sum11 = sl1[1]; sum12 = sl1[2];
            sum20 = sl2[0]; sum21 = sl2[1]; sum22 = sl2[2];

        } else { //2 samples per block (vertically stretched)

            int const step_y = (feature_height - 1) / 4;

            unsigned char const *const sl0 = image_data + step_y * image_step,
                                *const sl1 = image_data + (feature_height - step_y - 1) * image_step,
                                *const sl2 = sl0 + feature_height * image_step,
                                *const sl3 = sl1 + feature_height * image_step,
                                *const sl4 = sl2 + feature_height * image_step,
                                *const sl5 = sl3 + feature_height * image_step;

            sum00 = sl0[0] + sl1[0]; sum01 = sl0[1] + sl1[1]; sum02 = sl0[2] + sl1[2];
            sum10 = sl2[0] + sl3[0]; sum11 = sl2[1] + sl3[1]; sum12 = sl2[2] + sl3[2];
            sum20 = sl4[0] + sl5[0]; sum21 = sl4[1] + sl5[1]; sum22 = sl4[2] + sl5[2];

        }

    } else {

        int const step_x = (feature_width  - 1) / 4;

        int const x1 = step_x,
                  x2 = feature_width - step_x - 1,
                  x3 = x1 + feature_width,
                  x4 = x2 + feature_width,
                  x5 = x3 + feature_width,
                  x6 = x4 + feature_width;

        if(feature_height == 1) { //2 samples per block (horizontally stretched)

            unsigned char const *const sl0 = image_data,
                                *const sl1 = sl0 + image_step,
                                *const sl2 = sl1 + image_step;

            sum00 = sl0[x1] + sl0[x2]; sum01 = sl0[x3] + sl0[x4]; sum02 = sl0[x5] + sl0[x6];
            sum10 = sl1[x1] + sl1[x2]; sum11 = sl1[x3] + sl1[x4]; sum12 = sl1[x5] + sl1[x6];
            sum20 = sl2[x1] + sl2[x2]; sum21 = sl2[x3] + sl2[x4]; sum22 = sl2[x5] + sl2[x6];

        } else { //Large blocks are sampled using 4 samples per block

            int const step_y = (feature_height - 1) / 4;

            unsigned char const *const sl0 = image_data + step_y * image_step,
                                *const sl1 = image_data + (feature_height - step_y - 1) * image_step,
                                *const sl2 = sl0 + feature_height * image_step,
                                *const sl3 = sl1 + feature_height * image_step,
                                *const sl4 = sl2 + feature_height * image_step,
                                *const sl5 = sl3 + feature_height * image_step;

            sum00 = sl0[x1] + sl0[x2] + sl1[x1] + sl1[x2];
            sum01 = sl0[x3] + sl0[x4] + sl1[x3] + sl1[x4];
            sum02 = sl0[x5] + sl0[x6] + sl1[x5] + sl1[x6];

            sum10 = sl2[x1] + sl2[x2] + sl3[x1] + sl3[x2];
            sum11 = sl2[x3] + sl2[x4] + sl3[x3] + sl3[x4];
            sum12 = sl2[x5] + sl2[x6] + sl3[x5] + sl3[x6];

            sum20 = sl4[x1] + sl4[x2] + sl5[x1] + sl5[x2];
            sum21 = sl4[x3] + sl4[x4] + sl5[x3] + sl5[x4];
            sum22 = sl4[x5] + sl4[x6] + sl5[x5] + sl5[x6];

        }
    }

    /// Code below is for full feature calculation. Very slow, but almost no difference in detection quality
    /*
    unsigned char const *sl0 = image_data,
                        *sl1 = sl0 + feature_height * image_step,
                        *sl2 = sl1 + feature_height * image_step;

    for(int y = 0; y < feature_height; ++y) {
        for(int x = 0; x < feature_width; ++x) {
            sum00 += sl0[x]; sum01 += sl0[x + feature_width]; sum02 += sl0[x + feature_width * 2];
            sum10 += sl1[x]; sum11 += sl1[x + feature_width]; sum12 += sl1[x + feature_width * 2];
            sum20 += sl2[x]; sum21 += sl2[x + feature_width]; sum22 += sl2[x + feature_width * 2];
        }
        sl0 += image_step;
        sl1 += image_step;
        sl2 += image_step;
    }
    */

    //Two's complement arithmetic required!

    unsigned int const sign = 1 << 31;

    int const subset_index =
        ( ( ( (unsigned int)~(sum00 - sum11) ) & sign ) >> 29 ) |
        ( ( ( (unsigned int)~(sum01 - sum11) ) & sign ) >> 30 ) |
        (   ( (unsigned int)~(sum02 - sum11) )          >> 31 ) ;

    int const bit_index =
        ( ( ( (unsigned int)~(sum12 - sum11) ) & sign ) >> 27 ) |
        ( ( ( (unsigned int)~(sum22 - sum11) ) & sign ) >> 28 ) |
        ( ( ( (unsigned int)~(sum21 - sum11) ) & sign ) >> 29 ) |
        ( ( ( (unsigned int)~(sum20 - sum11) ) & sign ) >> 30 ) |
        (   ( (unsigned int)~(sum10 - sum11) )          >> 31 ) ;

    return (node->subsets[subset_index] >> bit_index) & 1;
}

/**
 * Classify single image position as object or not_object.
 * This function works as virtual machine interpreting instructions stored in
 * list pointed by "node" variable. It can understand 3 instructions:
 * NODE_DECISION: calculate specified feature value and decrease object_score if
 *   this value is in specified subset.
 * NODE_STAGE: compare object_score accumulated so far with specified threshold.
 *   if value is less than threshold then return 0 otherwise continue.
 * NODE_FINAL: return 1.
 * For performance reasons it is supposed that first node is always
 * NODE_DECISION, and two NODE_STAGE nodes are never go in succession.
 * @param node: classifier data;
 * @param image_data: position in memory where to sample image data
 * @param image_step: step from current image line to the next image line
 * @return Non-zero for positive classification, zero otherwise.
 */
static int classify (
    char const *node,
    unsigned char const *const image_data,
    int const image_step
) {
    //First node is always NODE_DECISION
    int object_score = ((EpNodeDecision const *)node)->score &
        -calc_lbp_decision(image_data, image_step, (EpNodeDecision const *)node);
    node += sizeof(EpNodeDecision);

    while(1) {
        if(!*node) { //NODE_DECISION
            object_score += ((EpNodeDecision const *)node)->score &
                -calc_lbp_decision(image_data, image_step, (EpNodeDecision const *)node);
            node += sizeof(EpNodeDecision);
        } else { //NODE_STAGE
            if(object_score < ((EpNodeStage *)node)->threshold)
                return 0;
            node += sizeof(EpNodeStage);

            if(*node)
                return 1; //NODE_FINAL

            //NODE_DECISION
            object_score = ((EpNodeDecision const *)node)->score &
                -calc_lbp_decision(image_data, image_step, (EpNodeDecision const *)node);
            node += sizeof(EpNodeDecision);
        }
    }

    return 0; //This point is unreachable
}

/**
 * Perform single-scale object detection
 * @param image: Image to scan.
 * @param classifier: Classifier to use.
 * @param objects: Detections will be added to this list.
 * @param scale: Size and coordinates of resulting rectangles will be multiplied by this factor.
 * @param offset_x: X coordinate of resulting rectangles will be offset by this values.
 * @param offset_y: Y coordinate of resulting rectangles will be offset by this values.
 * @param scan_mode: which pixels should be tested. @see EpScanMode
 */
static void detect_single_scale_host (
    EpImage             const *const image,
    EpCascadeClassifier const *      classifier,
    EpRectList                *const objects,
    float                      const scale,
    int                        const offset_x,
    int                        const offset_y,
    EpScanMode                 const scan_mode
) {
    /*{
        cv::Mat const cv_image(image->height, image->width, CV_8UC1, image->data, image->step);
        cv::imshow("Debug", cv_image);
        cv::waitKey(0);
    }*/

    char const *node = classifier->data;

    int const window_width  = ((EpNodeMeta const *)node)->window_width,
              window_height = ((EpNodeMeta const *)node)->window_height;

    int const process_width  = image->width  + 1 - window_width,
              process_height = image->height + 1 - window_height;

    float const detection_width  = window_width  * scale,
                detection_height = window_height * scale;

    node += sizeof(EpNodeMeta); //Skipping initial META node

    int const image_step = image->step;

    //OpenCV has this hack:
    //int step = scale > 2.0f ? 1 : 2;
    //We do not like it. Instead we use checkerboard scanning pattern.
    //Required calculations are almost doubled, but detection of small objects is better, and all pyramid levels are equal

    #pragma omp parallel for schedule(dynamic)
    for(int y = 0; y < process_height; ++y) {
        unsigned char const *const scan_line = image->data + y * image_step;

        int const x_start = scan_mode == SCAN_FULL ? 0 : (y + scan_mode) & 1;
        int const x_step = scan_mode == SCAN_FULL ? 1 : 2;

        for(int x = x_start; x < process_width; x += x_step) {
			if (classify(node, scan_line + x, image_step)) {
                #pragma omp critical(add_face)
                ep_rect_list_add (
                    objects,
                    x * scale + offset_x,
                    y * scale + offset_y,
                    detection_width,
                    detection_height
                );
            }
        }
    }
}

/**
 * Returns sequence
 * 8.0/8, 8.0/7, 8.0/6, 8.0/5, 16.0/8, 16.0/7, 16.0/6, 16.0/5, 32.0/8, 32.0/7, 32.0/6, 32.0/5, ...
 * needed to convert coordinates and object sizes detected at pyramid level image_index into
 * source image coordinates.
 */
static float convert_image_index_to_scale(int const image_index)
{
	return (float)( 8 << (image_index / 4) ) / ( 8 - (image_index % 4) );
}

/**
 * Process detection results.
 * @param objects          : Processed detections will be added here;
 * @param tasks            : Pointer to list of tasks (tiles) to process
 * @param images_properties: Pointer to list of images (pyramid scales) properties
 * @param window_width     : Width of classifier window (it is supposed that classifier used by core is known);
 * @param window_height    : Height of classifier window (it is supposed that classifier used by core is known);
 * @param offset_x         : Offset of x after scaling
 * @param offset_y         : Offset of y after scaling
 * @return total number of detections processed.
 */
static int process_results (
    EpRectList       *const objects,
    EpTaskList const *const tasks,
    EpImgList  const *const images_properties,
    int               const window_width,
    int               const window_height,
    int               const offset_x,
    int               const offset_y
) {
    int total_objects_count = 0;

    for(int i = 0; i < tasks->count; ++i) {
        EpTaskItem const *const task = tasks->data + i;

        int const image_index   = task->image_index;
        int const objects_count = task->items_count;
        int const tile_offset   = task->offset;

        int const image_step = images_properties->data[image_index].step;
        int const tile_x = tile_offset % image_step;
        int const tile_y = tile_offset / image_step;

        float const scale = convert_image_index_to_scale(image_index);
        float const object_width  = window_width  * scale;
        float const object_height = window_height * scale;

        assert(objects_count <= MAX_DETECTIONS_PER_TILE);

        for(int j = 0; j < objects_count; ++j) {
            int const object_pos_packed = task->objects[j];
            int const object_rel_x = object_pos_packed & 65535;
            int const object_rel_y = object_pos_packed >> 16;

            float const object_abs_x = (tile_x + object_rel_x) * scale + offset_x;
            float const object_abs_y = (tile_y + object_rel_y) * scale + offset_y;
            ep_rect_list_add(objects, object_abs_x, object_abs_y, object_width, object_height);
        }

        total_objects_count += objects_count;
    }

    return total_objects_count;
}


/**
 * Parse task on core times and output times in log_file
 * @param log_file  : name of result file
 * @param scale_time: time of images scaling
 * @param wait_time : time of host wait of detection
 * @param num_cores : count of working cores
 * @param timers    : array of cores timers
 */
static void time_log(
        char       const * const log_file,
        double     const         scale_time,
        double     const         wait_time,
        int        const         num_cores,
        EpTimerBuf const * const timers
) {
    FILE *f = fopen(log_file, "wt");
    fprintf(f, "------- Timers result in seconds ------\r\n\r\n");
    fprintf(f, "Scale time:               %lf\r\n", scale_time / 1000000);
    fprintf(f, "Host detection wait time: %lf\r\n", wait_time / 1000000);
    fprintf(f, "\r\nWork times per cores\r\n");
    fprintf(f, "=============================================\r\n");

    const double core_timer_freq = 1000000.0 * CORE_FREQUENCY;
    double total_cores_time = 0;
    for (int i = 0; i < num_cores; ++i) {
        double cur_time = timers[i].value / core_timer_freq * (1 << TIMER_VALUE_SHIFT);
        fprintf(f, "\t Core #%d:\t%lf\r\n", timers[i].core_id, cur_time);
        total_cores_time += cur_time;
    }

    fprintf(f, "=============================================\r\n");
    fprintf(f, "Average cores time: %lf\r\n", total_cores_time / num_cores);
    fprintf(f, "Total cores time: %lf\r\n", total_cores_time);

    fclose(f);
}

/**
 * Add in task list tasks from image.
 *
 * @param scan_mode    : which pixels to test (@see EpScanMode);
 * @param img_list     : list of images properties;
 * @param img_index    : index of current image;
 * @param window_width : detection window width;
 * @param window_height: detection window height;
 * @param task_buf     : task list;
 */

static void add_tasks_for_image(
        EpScanMode   const scan_mode,
        EpImgList  * const img_list,
        int          const img_index,
        int          const window_width,
        int          const window_height,
        EpTaskList * const task_buf
) {
    int tiles_ver;
    int tiles_hor;
    EpImageProp* img_prop = img_list->data + img_index;

    int const overlap_width = window_width  - 1,
             overlap_height = window_height - 1;

    //Corrected image width and height convenient for calculations
    int image_width  = img_prop->width  - overlap_width ,
        image_height = img_prop->height - overlap_height;

    //Ideally we have following equalities:
    // tile_width  * tiles_hor + overlap_width  * (tiles_hor - 1) == image_width
    // tile_height * tiles_ver + overlap_height * (tiles_ver - 1) == image_height
    //But it may happen that image will not be dividible by the tile size.
    //In this case we need to produce tiles slightly different in sizes to
    //  cover the whole image

    if(image_height < image_width) {
        tiles_ver = divide_round(image_height, RECOMMENDED_TILE_SIZE - overlap_height);
        if(!tiles_ver) tiles_ver = 1;

        //Tiles will have heights max_tile_height and, sometimes, max_tile_height - 1
        int const max_tile_height = divide_up(image_height + overlap_height * tiles_ver, tiles_ver);

        //Maximal allowed tile step to not exceed max_tile_area
        int const max_tile_step = round_down_to_8n( round_down_to_8n(MAX_TILE_BYTES / max_tile_height) - overlap_width) + overlap_width;

        tiles_hor = divide_up(image_width, max_tile_step - overlap_width);
    } else {
        tiles_hor = divide_round(image_width, RECOMMENDED_TILE_SIZE - overlap_width);
        if(!tiles_hor) tiles_hor = 1;

        //Tiles will have widths max_tile_step and, sometimes, max_tile_step - 8
        int const max_tile_step = round_up_to_8n(round_up_to_8n(divide_up(image_width + overlap_width * tiles_hor, tiles_hor) - overlap_width) + overlap_width);

        //Maximal allowed tile height to not exceed max_tile_area
        int const tile_height = MAX_TILE_BYTES / max_tile_step;

        tiles_ver = divide_up(image_height, tile_height - overlap_height);
    }
    const int num_tiles = tiles_hor * tiles_ver;

    for(int tile_index = 0; tile_index < num_tiles; ++tile_index) {
            int const tile_y = tile_index / tiles_hor,
                      tile_y1 = divide_round(image_height *  tile_y     , tiles_ver),
                      tile_y2 = divide_round(image_height * (tile_y + 1), tiles_ver) + overlap_height;

            int const tile_height = tile_y2 - tile_y1;

            int const tile_x = tile_index % tiles_hor,
                      tile_x1 = round_to_8n( divide_round(image_width *  tile_x     , tiles_hor) ),
                      tile_x2 = tile_x + 1 == tiles_hor ?
                                image_width + overlap_width :
                                round_to_8n( divide_round(image_width * (tile_x + 1), tiles_hor) ) + overlap_width;

            int const tile_width = tile_x2 - tile_x1,
                      tile_step  = round_up_to_8n(tile_width);

            assert(tile_step * tile_height <= MAX_TILE_BYTES);

            ep_task_list_add (
                task_buf,
                tile_x1 + tile_y1 * img_prop->step,
                tile_width,
                tile_height,
                tile_step,
                scan_mode == SCAN_FULL ? SCAN_FULL : (tile_x1 + tile_y1 + scan_mode) & 1,
                0,
                img_index
            );
    }
}

/**
 * Multiscale object detection
 *
 * Image is iteratively scaled down until it became less than native object size.
 * On each scale detection is performed.
 *
 * @param image     : Image to process (pointer to valid image structure).
 * @param classifier: Classifier to use (pointer to valid classifier structure).
 * @param objects   : Detections will be added to this list (pointer to valid rectangles list structure).
 * @param scan_mode : Which image pixels to test; @see EpScanMode.
 * @param num_cores : Number of cores in cores list.
 * @param log_file  : Name of log file. Pass NULL to disable log file and debug output.
 *
 * @return ERR_SUCCESS: successful detection;
 *         ERR_ARGUMENT: empty image, or invalid classifier, or unknown detection_mode, or unknown scan_mode.
 *         ERR_MEMORY: cannot allocate required memory (memory checks are not implemented yet).
 *         ERR_OTHER: classifier is too large and cannot be uploaded to core.
 */
EpErrorCode ep_detect_multi_scale_device (
    EpImage                   *const image,
    EpCascadeClassifier const *const classifier,
    EpRectList                *const objects,
    EpScanMode                 const scan_mode,
    int                        const num_cores,
    char                const *const log_file
) {
    if( ep_classifier_check(classifier) )
        return ERR_ARGUMENT; //Wrong classifier

    if( ep_image_is_empty(image) )
        return ERR_ARGUMENT; //Wrong image

    int const window_width = ((EpNodeMeta const *)classifier->data)->window_width ,
             window_height = ((EpNodeMeta const *)classifier->data)->window_height;

    if(image->width < window_width || image->height < window_height)
        return ERR_SUCCESS; //Image is too small; no detections

    int const blocks_x = image->width  / 8,
              blocks_y = image->height / 8;

    EpImage img8 = *image;
    EpImage img7 = ep_image_create(blocks_x * 7, blocks_y * 7);
    EpImage img6 = ep_image_create(blocks_x * 6, blocks_y * 6);
    EpImage img5 = ep_image_create(blocks_x * 5, blocks_y * 5);

    *image = ep_image_create_empty();

    double time_scale = 0.0;
    int64 time_start_scale = cvGetTickCount();
    int offset_x, offset_y;
    scale8765(&img8, &img7, &img6, &img5, &offset_x, &offset_y);
    time_scale += (cvGetTickCount() - time_start_scale) / cvGetTickFrequency();





	/*
	\D4\F6\BC\D3\C6\F4\B6\AF\B6\E0\BA\CB
	*/
	static ep_context_t ee;
	ep_context_t *e = NULL;
	e = &ee;
	e_init(NULL);
	e_reset_system();
	e_get_platform_info(&e->eplat);
	e_alloc(&e->emem, BUF_OFFSET, sizeof(EpDRAMBuf));
	
	e_open(&e->edev, 0, 0, ROWS, COLS);

	printf("load srec! ROWS=%d, COLS=%d\n", ROWS, COLS);

	if (e_load_group("epiphany.elf", &e->edev, 0, 0, ROWS, COLS, E_FALSE) == E_ERR)
	//if (e_load("epiphany.srec", &e->edev, 0, 0, E_TRUE) == E_ERR)
	{
		perror("e_load failed");
		return -1;
	}


    	// wake-up eCore
/*
	int row, col;
	for (row = 0; row < ROWS; row++)
	{
		for (col = 0; col < COLS; col++)
		{
			e_resume(&e->edev, row, col);
		}
	}
*/




    // 1 - build shared memory buffer
    //    1.1 - copy images, build images properties
    EpImgList imgs = ep_img_list_create_empty(0);

    if(log_file) printf("WRITING DATA TO SHARED MEMORY\n");

    int data_amount;
    while(1) {
        if(img8.width < window_width || img8.height < window_height) break;
        ep_img_list_add(&imgs, img8.step, img8.width, img8.height);
        if(log_file) { printf("Sending image %dx%d...", img8.width, img8.height); fflush(stdout); }
printf("write!\n");
		data_amount = e_write(&e->emem, 0, 0, offsetof(EpDRAMBuf, imgs_buf) + imgs.prev_offset, img8.data, img8.step * img8.height);
printf("write1\n");
        if(log_file) printf(" Image sent: %d bytes.\n", data_amount);

        if(img7.width < window_width || img7.height < window_height) break;
        ep_img_list_add(&imgs, img7.step, img7.width, img7.height);
        if(log_file) { printf("Sending image %dx%d...", img7.width, img7.height); fflush(stdout); }
printf("write!\n");
		data_amount = e_write(&e->emem, 0, 0,  offsetof(EpDRAMBuf, imgs_buf) + imgs.prev_offset, img7.data, img7.step * img7.height);
printf("write2\n");
        if(log_file) printf(" Image sent: %d bytes.\n", data_amount);

        if(img6.width < window_width || img6.height < window_height) break;
        ep_img_list_add(&imgs, img6.step, img6.width, img6.height);
        if(log_file) { printf("Sending image %dx%d...", img6.width, img6.height); fflush(stdout); }
printf("write!\n");
		data_amount = e_write(&e->emem, 0, 0, offsetof(EpDRAMBuf, imgs_buf) + imgs.prev_offset, img6.data, img6.step * img6.height);
printf("write3\n");
        if(log_file) printf(" Image sent: %d bytes.\n", data_amount);

        if(img5.width < window_width || img5.height < window_height) break;
        ep_img_list_add(&imgs, img5.step, img5.width, img5.height);
        if(log_file) { printf("Sending image %dx%d...", img5.width, img5.height); fflush(stdout); }
printf("write!\n");
		data_amount = e_write(&e->emem, 0, 0,offsetof(EpDRAMBuf, imgs_buf) + imgs.prev_offset, img5.data, img5.step * img5.height);
printf("write4\n");
        if(log_file) printf(" Image sent: %d bytes.\n", data_amount);

        time_start_scale = cvGetTickCount();
        scale21_realloc(&img8);
        scale21_realloc(&img7);
        scale21_realloc(&img6);
        scale21_realloc(&img5);
        time_scale += (cvGetTickCount() - time_start_scale) / cvGetTickFrequency();
    }

    if(log_file) { printf("Sending image properties..."); fflush(stdout); }
printf("write!\n");
	data_amount = e_write(&e->emem, 0, 0,offsetof(EpDRAMBuf, imgs_prop), imgs.data, imgs.count * sizeof(EpImageProp));
printf("write5\n");
    if(log_file) printf(" Data sent: %d bytes.\n", data_amount);

    //    1.2 - copy classifier
    if(log_file) { printf("Sending classifier..."); fflush(stdout); }
printf("write!\n");
	data_amount = e_write(&e->emem, 0, 0, offsetof(EpDRAMBuf, buf_classifier), classifier->data, round_up_to_8n(classifier->size));
printf("write6\n");
    if(log_file) printf(" Classifier sent: %d bytes.\n", data_amount);

    //    1.3 - build task list
    EpTaskList tasks = ep_task_list_create_empty();

    for(int i = 0; i < imgs.count; ++i)
        add_tasks_for_image(scan_mode, &imgs, i, window_width, window_height, &tasks);

    EpControlInfo control_info = {tasks.count, 0, 0, num_cores, 0};

    if(log_file) { printf("Sending task list..."); fflush(stdout); }
	data_amount = e_write(&e->emem, 0, 0, offsetof(EpDRAMBuf, tasks), tasks.data, tasks.count * sizeof(EpTaskItem));
    if(log_file) printf(" Task list sent: %u bytes.\n", data_amount);

    if(log_file) { printf("Sending control flags..."); fflush(stdout); }
	data_amount = e_write(&e->emem, 0, 0, offsetof(EpDRAMBuf, control_info), &control_info, sizeof(EpControlInfo));
    if(log_file) printf(" Data sent: %u bytes.\n", data_amount);

    













if(log_file) { printf("WAITING FOR CORES TO FINISH..."); fflush(stdout); }


    // 2 - wait end of detection
    
        e_read(&e->emem, 0, 0, offsetof(EpDRAMBuf, control_info), &control_info, sizeof(EpControlInfo));
        printf("unused: %d, start_cores: %d, task_finished: %d, tasks.count: %d\n",control_info.unused, control_info.start_cores, control_info.task_finished,tasks.count); 
	//e_start(&e->edev, 0, 0);
	e_start_group(&e->edev);
	int64 const time_start_waiting = cvGetTickCount();
#ifndef DEVICE_EMULATION
    while(1) {
        e_read(&e->emem, 0, 0, offsetof(EpDRAMBuf, control_info), &control_info, sizeof(EpControlInfo));
        if(control_info.task_finished == tasks.count)
            break;
        //printf("unused: %d, start_cores: %d, task_finished: %d, tasks.count: %d\n",control_info.unused, control_info.start_cores, control_info.task_finished,tasks.count); 
        //sleep(1);
    }
#else //DEVICE_EMULATION
    device_process_tasks();
#endif//DEVICE_EMULATION

    double const wait_time = (cvGetTickCount() - time_start_waiting) / cvGetTickFrequency();

    if(log_file) printf(" CORES FINISHED IN %lf SECONDS.\n", wait_time / 1000000);

    if(log_file) { printf("Downloading results..."); fflush(stdout); }
    // 3 - download result and analyze detections
	data_amount = e_read(&e->emem, 0, 0, offsetof(EpDRAMBuf, tasks), tasks.data, sizeof(EpTaskItem)* tasks.count);
    if(log_file) printf(" Results downloaded: %d bytes.\n", data_amount);
    process_results(objects, &tasks, &imgs, window_width, window_height, offset_x, offset_y);

    // 4 - download timers values
    if(log_file) {
        printf("Downloading timers..."); fflush(stdout);
        EpTimerBuf timers[num_cores];
		data_amount = e_read(&e->emem, 0, 0, offsetof(EpDRAMBuf, timers), timers, sizeof(EpTimerBuf)* num_cores);
        printf(" Timers downloaded: %d bytes.\n", data_amount);
        time_log(log_file, time_scale, wait_time, num_cores, timers);
    }








	/*
	\CAͷ\C5\C4ڴ\E6
	*/





	e_close(&e->edev);
	e_free(&e->emem);
	e_finalize();



    ep_task_list_release(&tasks);
    ep_img_list_release(&imgs);

    ep_image_release(&img5);
    ep_image_release(&img6);
    ep_image_release(&img7);
    ep_image_release(&img8);

    return ERR_SUCCESS;
}

EpErrorCode ep_detect_multi_scale_host (
    EpImage                   *const image,
    EpCascadeClassifier const *const classifier,
    EpRectList                *const objects,
    EpScanMode                 const scan_mode
) {
    if( ep_classifier_check(classifier) )
        return ERR_ARGUMENT; //Wrong classifier

    if( ep_image_is_empty(image) )
        return ERR_ARGUMENT; //Wrong image

    int const window_width = ( (EpNodeMeta const *)classifier->data )->window_width ,
             window_height = ( (EpNodeMeta const *)classifier->data )->window_height;

    if(image->width < window_width || image->height < window_height)
        return ERR_SUCCESS; //Image is too small; no detections

    int const blocks_x = image->width  / 8,
              blocks_y = image->height / 8;

    EpImage img8 = *image;
    EpImage img7 = ep_image_create(blocks_x * 7, blocks_y * 7);
    EpImage img6 = ep_image_create(blocks_x * 6, blocks_y * 6);
    EpImage img5 = ep_image_create(blocks_x * 5, blocks_y * 5);

    *image = ep_image_create_empty();

    int offset_x, offset_y;
    scale8765(&img8, &img7, &img6, &img5, &offset_x, &offset_y);

    int image_index = 0;
    float scale;

    while(1) {
        if(img8.width < window_width || img8.height < window_height) break;
        scale = convert_image_index_to_scale(image_index    );
        detect_single_scale_host(&img8, classifier, objects, scale, offset_x, offset_y, scan_mode);

        if(img7.width < window_width || img7.height < window_height) break;
        scale = convert_image_index_to_scale(image_index + 1);
        detect_single_scale_host(&img7, classifier, objects, scale, offset_x, offset_y, scan_mode);

        if(img6.width < window_width || img6.height < window_height) break;
        scale = convert_image_index_to_scale(image_index + 2);
        detect_single_scale_host(&img6, classifier, objects, scale, offset_x, offset_y, scan_mode);

        if(img5.width < window_width || img5.height < window_height) break;
        scale = convert_image_index_to_scale(image_index + 3);
        detect_single_scale_host(&img5, classifier, objects, scale, offset_x, offset_y, scan_mode);

        scale21(&img8, &img8);
        scale21(&img7, &img7);
        scale21(&img6, &img6);
        scale21(&img5, &img5);

        image_index += 4;
    }

    ep_image_release(&img5);
    ep_image_release(&img6);
    ep_image_release(&img7);
    ep_image_release(&img8);

    return ERR_SUCCESS;
}
