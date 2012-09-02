#ifndef _TTRACK_HPP_
#define _TTRACK_HPP_

/**
 * @mainpage Surgical Instrument Tracking
 * 
 * Information about the project...
 */

#include "headers.hpp"
#include "tracker.hpp"
#include "detect.hpp"
#include "handler.hpp"
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>

/**
 * @namespace ttrk
 * This is the namespace for the tool tracking project.
 */

namespace ttrk{

  /**
   * @class TTrack
   * @brief The interface class for the project. Handles the interaction between all worker classes.
   *
   * This is the interface class of the project. It owns the tracker and the detector as well as
   * performing tasks like saving current frames for validation and the initial interaction with 
   * the filesystem.
   *
   */
  class TTrack{
    
  public:
    
    static void Destroy();
    void CleanUp();
    ~TTrack();
    
    /**
     * A factory method for getting a reference to the singleton TTrack class.
     */
    static TTrack &Instance(); 

    /**
     * A method to start running the main method of the class. Loops getting a new frame from the video file, classifying it and then detecting the 
     * instruments in it. Per-frame pose is saved in out_posefile file and video in the out_videofile file.
     * 
     */
    void RunVideo();
    
    /**
     * A method to start running the main method of the class. Same features as RunVideo but it inputs still frames from a directory
     */
    void RunImages();
    
    /**
     * The main method of the class. Loops calling the methods of the Tracker and the Detector.
     */
    void Run();


     /**
     * Grab a ptr to a new frame. This is the interface to use if reading from images or reading from a videofile.
     * @return The ptr to the frame.
     */
    cv::Mat *GetPtrToNewFrame();

    /**
     *
     */
    cv::Mat *GetPtrToClassifiedFrame();

    /**
     * A method for saving the current frame in output directory. 
     */
    void SaveFrame();

    /**
     * Draw the model at the current pose onto the classified image ready for it to be saved
     */
    void DrawModel(cv::Mat *image);

    /**
     * Setup the directory tree structure containing the root directory of the data sets
     * as well as the directory where output files are to be saved
     * @param root_dir The root training data directory.
     */
    void SetUpDirectoryTree(const std::string &root_dir);
    
   

  protected:
    
    /**
     * Save the debugging images, if required
     */
    void SaveDebug() const;

    Tracker tracker_; /**< The class responsible for finding the instrument in the image. */
    Detect detector_; /**< The class responsible for classifying the pixels in the image. */
    Handler *handler_; /**< Pointer to either an ImageHandler or a VideoHandler which handles getting and saving frames with a simple interface */
    
    cv::Mat *frame_; /**< A pointer to the current frame that will be passed from the classifier to the tracker. */
    
    std::string root_dir_; /**< A string containing the root directory for the data in use. */
    

  private:

    TTrack();
    TTrack(const TTrack &);
    TTrack &operator=(const TTrack &);
    static bool constructed_;
    static TTrack *instance_;

  };


}

#endif //_TTRACK_H_
