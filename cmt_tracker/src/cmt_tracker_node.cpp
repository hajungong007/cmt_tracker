//This is the base file that will handle all the relevant part of processing

//Messages for holding tracker and face locations
#include <cmt_tracker/Tracker.h>
#include <cmt_tracker/Trackers.h>
#include <cmt_tracker/Face.h>
#include <cmt_tracker/Faces.h>
#include <cmt_tracker/TrackedImages.h>

//OpenCV libraries
#include <opencv2/core/core.hpp>
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

//ROS - OpenCV libraries
#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
//#include <sensor_msgs/ImageConstPtr.h>

//Service call to reset the values of the images in the system
#include <cmt_tracker/Clear.h>

#include <cmt_tracker/TrackerConfig.h>
#include <dynamic_reconfigure/server.h>

#include <stdlib.h>
#include <time.h>

// CMT libraryies
#include "CMT.h"
#include "gui.h"
#ifdef __GNUC__
#include <getopt.h>
#else
#include "getopt/getopt.h"
#endif
// CMT Libraries End

//Threading Libraries
#include <boost/thread.hpp>

#include <sstream>

#define SSTR( x ) dynamic_cast< std::ostringstream & >(( std::ostringstream() << std::dec << x ) ).str()

/**
The requirements of the this class:
1. This the function spawns the CMT instances.
  1.1 This is the fucntion that initalized.
  1.2 This is the one that publishes the location of the tracker results to the system.
  

2. This is the fucntion that holds parameters for the tracker configuration and the one that would be modified when changes are made the plugin.
  2.1 When a person changes the configuration in the parameter server; this fucntion must change the way it performs it's tasks.
  2.2 When the CMT is initialized it's  must get the parameters from the systme so that it would perfrom in house calculation to maintain decent results.
  2.3 The tracking results are maintained here and updated correspondingly.

3. THIS FUNCTION MUST BE THREADED.

What do i need to do?

Make this function handle the processing of the instances of cmt.
Make this function have nothing to do with any rules.
Make this function configurable from outside with parameters.
Suppress all the info coming from the cmt library. 

*/

using namespace cmt;

class Face_Detection{
  public:
  static std::vector<cv::Rect> facedetect(cv::Mat frame_gray)
  {
  cv::CascadeClassifier face_cascade;
  cv::CascadeClassifier eyes_cascade;
  std::vector<cv::Rect> faces;
  bool setup;
  if ( !face_cascade.load("/usr/share/opencv/haarcascades/haarcascade_frontalface_alt.xml" ) || !eyes_cascade.load("/usr/share/opencv/haarcascades/haarcascade_eye_tree_eyeglasses.xml" ))
    { setup = false;  };
    setup = true;
  if(setup)
  {

  face_cascade.detectMultiScale( frame_gray, faces, 1.05, 4, 0 | cv::CASCADE_SCALE_IMAGE, cv::Size(40, 40) );
  //Now add eyes if one desires

  }
  return faces;
  }
  static cmt_tracker::Trackers convert(std::vector<cv::Rect> faces)
  {
      cmt_tracker::Trackers tracker_description;
     for (size_t i = 0; i < faces.size(); i++)
        {

          cmt_tracker::Tracker face_description;
          face_description.pixel_lu.x = faces[i].x;
          face_description.pixel_lu.y = faces[i].y;
          //Now place coordinates to the value Z from the
          //depth camera.
          face_description.pixel_lu.z = 0;
          face_description.height.data = faces[i].height;
          face_description.width.data = faces[i].width;


          tracker_description.tracker_results.push_back(face_description);
        }
        return tracker_description;
   }
   static cmt_tracker::Trackers returnOverlapping(std::vector<cv::Rect> cmt_locations, cmt_tracker::Faces facelocs)
   {
    std::vector<cv::Rect> not_overlapped; 
    //This is wrote to avoid calling face detect algorithm multiple times in this application. So, we check for overlap between the cmt_locations 
    //and the facelocs (found from the face_locator node) and then if there are new values we push it note the rect and hold it in a new rect. 
    for (int i = 0; i < facelocs.faces.size(); i++)
    {
      cv::Rect area_overlap(facelocs.faces[i].pixel_lu.x, facelocs.faces[i].pixel_lu.y, facelocs.faces[i].width.data, facelocs.faces[i].height.data);
      bool no_overlap= false; 
      for (int j=0; j< cmt_locations.size(); j++)
      {
        //if it overlaps with the one then break. 
        no_overlap= (area_overlap & cmt_locations[j]).area() > 0; 
        if(no_overlap)
          break; 
      }
      if(!no_overlap)
        not_overlapped.push_back(area_overlap);
      //if doesn't overlap then break. 
    }
    return convert(not_overlapped);
   }
};

class TrackerCMT
{

//to hold the value of the image
  cv::Mat conversion_mat_;
  cv::Mat frame_gray;

  dynamic_reconfigure::Server<cmt_tracker::TrackerConfig> server;
  dynamic_reconfigure::Server<cmt_tracker::TrackerConfig>::CallbackType f;

  std::vector<cv::Mat> tracked_images;

  cv::CascadeClassifier face_cascade;
  cv::CascadeClassifier eyes_cascade;
  std::vector<cv::Rect> faces;
  std::vector<cv::Rect> eyes;

  ros::NodeHandle nh_;

  cmt_tracker::Tracker track_location;

  ros::ServiceServer clear_service;
  ros::ServiceServer image_service;

  image_transport::ImageTransport it_;
  image_transport::Subscriber image_sub_;
  image_transport::Publisher image_pub_;
  image_transport::Publisher image_face_pub;
  ros::Publisher tracker_locations_pub;
  ros::Publisher tracker_results_pub;

  ros::Subscriber face_results;
  ros::Subscriber tracker_subscriber;
  ros::Subscriber trackers_subscriber;
  ros::Subscriber face_subscriber;

  cmt_tracker::Tracker tracker_set;
  cmt_tracker::Faces face_locs;
  cmt_tracker::Trackers trackers_results;

  std::vector<CMT> cmt;
  std::vector<int> quality_of_tracker;

  std::vector<int> deletion_values;

  std::string tracking_method;
  bool setup;
  int last_count;

  double factor;
  

  std::vector<cv::Rect> locations_of_trackers; //for setting the tracking to higher levels.
  std::string subscribe_topic;
  sensor_msgs::ImagePtr masked_image;

  int frame_counters;
  int frame_previous;


  //Parameters for the CMT part of the code.
public:
  TrackerCMT() : it_(nh_)
  {
    
    if ( !face_cascade.load("/usr/share/opencv/haarcascades/haarcascade_frontalface_alt.xml" ) || !eyes_cascade.load("/usr/share/opencv/haarcascades/haarcascade_eye_tree_eyeglasses.xml" ))
    { setup = false;  };
    setup = true;
    nh_.getParam("camera_topic", subscribe_topic);
    last_count=0; 
    frame_counters = 0;
    frame_previous = 0;
    //To acquire the image that we will be doing processing on
    image_sub_ = it_.subscribe(subscribe_topic, 1, &TrackerCMT::imageCb, this);


    //Service to deal with the image systems.
    clear_service = nh_.advertiseService("clear", &TrackerCMT::clear, this);
    image_service = nh_.advertiseService("get_cmt_rects", &TrackerCMT::getTrackedImages, this);

    //To acquire the list of faces currently in track.
    face_subscriber = (nh_).subscribe("face_locations", 1, &TrackerCMT::list_of_faces_update, this);
    tracker_locations_pub = (nh_).advertise<cmt_tracker::Trackers>("tracking_locations", 10);


    //To acquire commands from this and other nodes to what to set to track.
    // masked_image = cv_bridge::CvImage(std_msgs::Header(), "mono", image_roi).toImageMsg();
    tracker_subscriber = (nh_).subscribe("tracking_location", 1, &TrackerCMT::set_tracker, this);
    tracker_subscriber = (nh_).subscribe("tracking_locations", 1, &TrackerCMT::set_trackers, this);
    image_pub_ = it_.advertise("/transformed/images", 1);
    image_face_pub = it_.advertise("/transformed/faces", 1);
    
    tracker_results_pub = nh_.advertise<cmt_tracker::Trackers>("tracker_results", 10);

    f = boost::bind(&TrackerCMT::callback, this, _1, _2);

    server.setCallback(f);
  }

  bool clear(cmt_tracker::Clear::Request &req, cmt_tracker::Clear::Response &res)
  {
    cmt.clear();
    quality_of_tracker.clear();
    if (cmt.size() == 0)
    {
       res.cleared= true;
    }
    else
    {
       res.cleared = false;
    }
    return true;
  }
  /*
  Request the images in the system.
  */
  bool getTrackedImages(cmt_tracker::TrackedImages::Request &req,
                        cmt_tracker::TrackedImages::Response &res)
  {
  //Now this get's the name of elements in one iteration in a single copy method.
      for (std::vector<CMT>::iterator v = cmt.begin(); v!= cmt.end(); ++v)
      {

        res.names.push_back((*v).name);

        sensor_msgs::ImagePtr masked_image= cv_bridge::CvImage(std_msgs::Header(), "mono8", (*v).imArchive).toImageMsg();


        //Why not publish the image here and see it's corresponding response regarding how to implement the best form of the system.
        image_face_pub.publish(masked_image);

        res.image.push_back(*masked_image);

      }
      return true;
  }
  /*
  This  function is a callback function that happens when an image update occurs.
  */
  void imageCb(const sensor_msgs::ImageConstPtr& msg)
  {
    //TODO why not just subscribe to the MONO image and skip all this handling. A way must be included to do that functionality. 
    try
    {
      // First let cv_bridge do its magic
      cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(msg, sensor_msgs::image_encodings::RGB8);
      conversion_mat_ = cv_ptr->image;
    }
    catch (cv_bridge::Exception& e)
    {
      try
      {
        // If we're here, there is no conversion that makes sense, but let's try to imagine a few first
        cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(msg);
        if (msg->encoding == "CV_8UC3")
        {
          // assuming it is rgb
          conversion_mat_ = cv_ptr->image;
        } else if (msg->encoding == "8UC1") {
          // convert gray to rgb
          cv::cvtColor(cv_ptr->image, conversion_mat_, CV_GRAY2RGB);
        }  else {
          // qWarning("ImageView.callback_image() could not convert image from '%s' to 'rgb8' (%s)", msg->encoding.c_str(), e.what());
          std::cerr << "Error in the conversion of the iamge" << std::endl;
          // ui_.image_frame->setImage(QImage());
          return;
        }
      }
      catch (cv_bridge::Exception& e)
      {
        std::cerr << "Error in the conversion of the image" << std::endl;
        // qWarning("ImageView.callback_image() while trying to convert image from '%s' to 'rgb8' an exception was thrown (%s)", msg->encoding.c_str(), e.what());
        // ui_.image_frame->setImage(QImage());
        return;
      }
    }
    //Do convert the image to gray scale as it's don't multiple times across other functions in the system.


    cv::cvtColor(conversion_mat_, frame_gray, CV_BGR2GRAY);

    cv::Mat im_gray = frame_gray.clone();
    //Now let's copy an additional mat to do additional removal.
    cv::Mat im_masked= frame_gray.clone();

    std::cout << "The number of tracker active is: " << cmt.size() << std::endl;
    int quality_update = 0;
    int count=0;
    deletion_values.clear();
    std::vector<cv::Rect> rectLocs; 
    for (std::vector<CMT>::iterator v = cmt.begin(); v != cmt.end(); ++v)
    {
      //Clear all the previous results relating to the tracker.

      cmt_tracker::Tracker tracker;
      if ((*v).initialized == true && !im_gray.empty())
      {
        tracker.inital_points.data = (*v).num_initial_keypoints;
        tracker.active_points.data = (*v).num_active_keypoints;
        tracker.tracker_name.data = (*v).name;

        if(!(*v).tracker_lost)
        {
        (*v).processFrame(im_gray,factor);
        cv::Rect rect = (*v).bb_rot.boundingRect();
        rectLocs.push_back(rect);



        quality_of_tracker[quality_update] = (*v).num_active_keypoints;
        quality_update++;
        //FILE_LOG(logDEBUG) << "Area ouptut is: " << rect.area();
        rect = rect & cv::Rect(0, 0, im_gray.size().width, im_gray.size().height);
        tracker.pixel_lu.x = rect.x;
        tracker.pixel_lu.y = rect.y;
        tracker.pixel_lu.z = 0;




        tracker.width.data = rect.width;
        tracker.height.data = rect.height;

          //FILE_LOG(logDEBUG) << "No suitable result";
        tracker.quality_results.data = true;
        trackers_results.tracker_results.push_back(tracker);

        }
        else
        {
          tracker.pixel_lu.x = 0;
          tracker.pixel_lu.y = 0;
          tracker.pixel_lu.z = 0;
          tracker.width.data = 0;
          tracker.height.data = 0;
          tracker.quality_results.data = false;
          trackers_results.tracker_results.push_back(tracker);
          deletion_values.push_back(count);
        }
      }
      count++;
    }


    tracker_results_pub.publish(trackers_results);
    

    //Now let's deal with removing elements.
    for (int i=0; i< deletion_values.size(); i++)
    {
      remove_tracker(deletion_values[i]);
    }
    //nh.setParam("tracker_updated","true");

    //Now let's deal with the rectangles of the system. It's done here because this would need to be refactored into another stream of code.
    // for (std::vector<CMT>::iterator v = cmt.begin(); v != cmt.end(); ++v)
    // {
    //   cv::Rect intersection_area= (*v).bb_rot.boundingRect();
    //   cv::Rect normailze = intersection_area & cv::Rect(0, 0, im_masked.size().width, im_masked.size().height);

    //   cv::Mat mask(normailze.size(), CV_16UC1);
    //   mask.setTo(cv::Scalar(5));
    //   mask.copyTo(im_masked(normailze));

    // }

    //Now let's detect faces in this area.
    // std::vector<cv::Rect> faces= Face_Detection::facedetect(im_masked);
    cmt_tracker::Trackers track_locs= Face_Detection::returnOverlapping(rectLocs,face_locs);
    tracker_locations_pub.publish(track_locs);
    trackers_results.tracker_results.clear();

  }
  void callback(cmt_tracker::TrackerConfig &config, uint32_t level)
  {
    factor = config.factor;
  }
  void list_of_faces_update(const cmt_tracker::Faces& faces_info)
  {
    face_locs.faces.clear();
    for (int i = 0; i < faces_info.faces.size(); i++)
    {
      face_locs.faces.push_back(faces_info.faces[i]);
    }
    //nh_.setParam("tracker_updated",true);
  }
  void set_tracker(const cmt_tracker::Tracker& tracker_location)
  {
    //A potentially high penality task is done here but it's to avoid latter dealing with uncorrectly set trackers.
    cv::Mat im_gray = frame_gray.clone(); //To avoid change when being run.
    cv::Rect rect(tracker_location.pixel_lu.x, tracker_location.pixel_lu.y, tracker_location.width.data, tracker_location.height.data );
    if (!im_gray.empty() && rect.area() > 50)
    {
      //Now there must be some way to hold back setting up new tracker;
      cmt.push_back(CMT());
      cmt.back().consensus.estimate_rotation = true;

      srand(time(NULL));

      int tracker_num;

      //check tracker id is unique;

      tracker_num = rand() % 100000;

      std::string tracker_name = SSTR(tracker_num) ;

      cmt.back().initialize(im_gray, rect, tracker_name);
      quality_of_tracker.push_back(cmt.back().num_initial_keypoints);
    }
    else
    {

      //FILE_LOG(logDEBUG) << "Not initialized";
    }
    nh_.setParam("tracker_updated","true");
  }
  void set_trackers(const cmt_tracker::Trackers& tracker_location)
  {
    for (int i=0; i< tracker_location.tracker_results.size(); i++)
    {
      set_tracker(tracker_location.tracker_results[i]);
    }
    nh_.setParam("tracker_updated","true");
  }
  void remove_tracker(int index)
  {
    cmt.erase(cmt.begin()+index);
    nh_.setParam("tracker_updated","true"); 
  }

};



int main(int argc, char** argv)
{
  ros::init(argc, argv, "cmt_tracker");
  TrackerCMT ic;
  ros::spin();
  return 0;
}