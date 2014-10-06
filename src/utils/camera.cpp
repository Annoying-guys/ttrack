#include <fstream>
#include <boost/filesystem.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <cinder/gl/gl.h>
#include <cinder/Camera.h>

#include "../../include/utils/camera.hpp"
#include "../../include/constants.hpp"
#include "../../include/utils/helpers.hpp"

using namespace ttrk;

MonocularCamera::MonocularCamera(const std::string &calibration_filename){

  cv::FileStorage fs;

  try{

    fs.open(calibration_filename,cv::FileStorage::READ); 
    fs["Camera_Matrix"] >> intrinsic_matrix_;
    fs["Distortion_Coefficients"] >> distortion_params_;

    cv::Mat image_dims;
    fs["Image_Dimensions"] >> image_dims;
    image_width_ = image_dims.at<int>(0);
    image_height_ = image_dims.at<int>(1);
    
  }catch(cv::Exception& e){

    std::cerr << "Error while reading from camara calibration file.\n" << e.msg << "\n";
    SAFE_EXIT();

  }

}

cv::Point2i MonocularCamera::ProjectPointToPixel(const cv::Point3d &point) const {
  cv::Point2d pt = ProjectPoint(point);
  return cv::Point2i(ttrk::round(pt.x),ttrk::round(pt.y));
}

cv::Mat MonocularCamera::GetUnprojectedImagePlane(const int width, const int height) {

  if (unprojected_image_.data != nullptr) {
    return unprojected_image_;
  }
  else{
    std::vector<cv::Vec2f> points,outpoints;
    points.reserve(width*height);
    for (int r = 0; r < height; ++r){
      for (int c = 0; c < width; ++c){
        points.push_back(cv::Vec2f(c, r));
      }
    }

    cv::undistortPoints(points, outpoints, intrinsic_params(), distortion_params());

    unprojected_image_ = cv::Mat(height, width, CV_32FC2);
    float *data = (float *)unprojected_image_.data;
    const int channels = 2;
    for (int r = 0; r < height; ++r){
      for (int c = 0; c < width; ++c){
        const int index = ((width*r) + c);
        data[index*channels] = outpoints[index][0];
        data[(index*channels) + 1] = outpoints[index][1];
      }
    }
  
    return unprojected_image_;
  }


}

cv::Point3d MonocularCamera::UnProjectPoint(const cv::Point2i &point) const {
  
  cv::Mat projected(1,1,CV_64FC2);
  projected.at<cv::Vec2d>(0,0) = cv::Vec2d(point.x,point.y);
  cv::Mat unprojected(1,1,CV_64FC2);
  
  cv::undistortPoints(projected, unprojected, intrinsic_matrix_, distortion_params_); 

  return cv::Point3d(unprojected.at<cv::Vec2d>(0,0)[0],unprojected.at<cv::Vec2d>(0,0)[1],1);

}

void MonocularCamera::SetGLProjectionMatrix() const {

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();

  const int near_clip_distance = GL_NEAR;
  const int far_clip_distance = GL_FAR;
  ci::Matrix44f gl_projection_matrix_;
  //setup openGL projection matrix
  gl_projection_matrix_.setToNull();
  gl_projection_matrix_.m00 = (float)intrinsic_matrix_.at<double>(0, 0);
  gl_projection_matrix_.m11 = (float)intrinsic_matrix_.at<double>(1, 1);
  gl_projection_matrix_.m02 = (float)-intrinsic_matrix_.at<double>(0, 2);
  gl_projection_matrix_.m12 = (float)-intrinsic_matrix_.at<double>(1, 2);
  gl_projection_matrix_.m22 = (float)(near_clip_distance + far_clip_distance);
  gl_projection_matrix_.m23 = (float)(near_clip_distance * far_clip_distance);
  gl_projection_matrix_.m32 = -1;

  glMultMatrixf(gl_projection_matrix_.m);

}

cv::Point2d MonocularCamera::ProjectPoint(const cv::Point3d &point) const {

  std::vector<cv::Point2d> projected_point;
  static cv::Mat rot = cv::Mat::eye(3,3,CV_64FC1);
  static cv::Mat tran = cv::Mat::zeros(3,1,CV_64FC1);
  
  cv::projectPoints(std::vector<cv::Point3d>(1,point),rot,tran,intrinsic_matrix_,distortion_params_,projected_point);
  if(projected_point.size() != 1) throw(std::runtime_error("Error, projected points size != 1.\n"));
  
  return projected_point.front();

}


StereoCamera::StereoCamera(const std::string &calibration_filename):rectified_(false),extrinsic_matrix_(4,4,CV_64FC1){

  if(!boost::filesystem::exists(boost::filesystem::path(calibration_filename)))
    throw(std::runtime_error("Error, could not find camera calibration file: " + calibration_filename + "\n"));

  cv::FileStorage fs;  

  try{

    cv::Mat image_dims;
    cv::Mat l_intrinsic, l_distortion;
    cv::Mat r_intrinsic, r_distortion;
    fs.open(calibration_filename, cv::FileStorage::READ);

    fs["Image_Dimensions"] >> image_dims;
    int image_width = image_dims.at<int>(0);
    int image_height = image_dims.at<int>(1);

    fs["Left_Camera_Matrix"] >> l_intrinsic;
    fs["Left_Distortion_Coefficients"] >> l_distortion;
    left_eye_.reset(new MonocularCamera(l_intrinsic, l_distortion, image_width, image_height));
    rectified_left_eye_.reset( new MonocularCamera );

    fs["Right_Camera_Matrix"] >> r_intrinsic;
    fs["Right_Distortion_Coefficients"] >> r_distortion;
    right_eye_.reset(new MonocularCamera(r_intrinsic, r_distortion, image_width, image_height));
    rectified_right_eye_.reset( new MonocularCamera );

    cv::Mat rotation(3,3,CV_64FC1),translation(3,1,CV_64FC1);
    fs["Extrinsic_Camera_Rotation"] >> rotation;
   
    fs["Extrinsic_Camera_Translation"] >> translation;

    for(int r=0;r<3;r++){
      for(int c=0;c<3;c++){
        extrinsic_matrix_.at<double>(r,c) = rotation.at<double>(r,c);
      }
      //if(r < 2)
      //  extrinsic_matrix_.at<double>(r,3) = -1 * translation.at<double>(r,0);
      //else
      extrinsic_matrix_.at<double>(r,3) = translation.at<double>(r,0);
    }

    extrinsic_matrix_(cv::Range(3,4),cv::Range::all()) = 0.0;
    extrinsic_matrix_.at<double>(3,3) = 1.0;

  }catch(cv::Exception& e){

    std::cerr << "Error while reading from camara calibration file.\n" << e.msg << "\n";
    SAFE_EXIT();

  }

}

void TestReproject(const cv::Mat &disparity_map, cv::Mat &reprojected_point_cloud, const cv::Mat &reprojection_matrix) {

  reprojected_point_cloud = cv::Mat(disparity_map.size(),CV_32FC3);

  for(int r=0;r<disparity_map.rows;r++){
    for(int c=0;c<disparity_map.cols;c++){

      cv::Vec3d &point = reprojected_point_cloud.at<cv::Vec3d>(r,c);

      double data[] = {r,c,disparity_map.at<float>(r,c),1.0};
      cv::Mat p(4,1,CV_64FC1,data);
      cv::Mat dp = reprojection_matrix * p;
      const double denom = dp.at<double>(3);
      point = cv::Vec3d( dp.at<double>(0)/denom, dp.at<double>(1)/denom, dp.at<double>(2)/denom);

    }
  }

}

void StereoCamera::SetupGLCameraFromRight() const{

  right_eye_->SetGLProjectionMatrix();

  ci::CameraPersp camP;

  ci::Vec3f eye_point(0, 0, 0);
  ci::Vec3f view_direction(0, 0, 1);
  ci::Vec3f world_up(0, -1, 0);

  ci::Matrix33f extrinsic_rotation;
  for (int r = 0; r < 3; ++r){
    for (int c = 0; c < 3; ++c){
      extrinsic_rotation.at(r, c) = extrinsic_matrix_.at<double>(c, r); //needs transposing as ci is column major
    }
  }

  view_direction = extrinsic_rotation * view_direction;
  world_up = extrinsic_rotation * world_up;

  camP.setEyePoint(ci::Vec3f(extrinsic_matrix_.at<double>(0, 3), extrinsic_matrix_.at<double>(1, 3), extrinsic_matrix_.at<double>(2, 3)));
  camP.setViewDirection(view_direction);
  camP.setWorldUp(world_up);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMultMatrixf(camP.getModelViewMatrix().m);


}

void StereoCamera::SetupGLCameraFromLeft() const{

  left_eye_->SetGLProjectionMatrix();

  ci::CameraPersp camP;

  ci::Vec3f eye_point(0, 0, 0);
  ci::Vec3f view_direction(0, 0, 1);
  ci::Vec3f world_up(0, -1, 0);

  camP.setEyePoint(eye_point);
  camP.setViewDirection(view_direction);
  camP.setWorldUp(world_up);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMultMatrixf(camP.getModelViewMatrix().m);

}

cv::Vec3d StereoCamera::ReprojectPointTo3D(const cv::Point2i &left, const cv::Point2i &right){

  int vertical_disparity = std::abs(left.y - right.y);
  if (vertical_disparity > 40){

    return cv::Vec3d(0,0,0);

  }else{
    int horizontal_disparity = left.x - right.x;
    cv::Mat to_reproject(4,1,CV_64FC1);
    to_reproject.at<double>(0) = left.x;
    to_reproject.at<double>(1) = left.y;
    to_reproject.at<double>(2) = horizontal_disparity;
    to_reproject.at<double>(3) = 1;
    cv::Mat projected = reprojection_matrix_ * to_reproject;
    if (projected.at<double>(3) == 0) projected.at<double>(3) = 0.1;
    return cv::Vec3d( projected.at<double>(0)/projected.at<double>(3),
                      projected.at<double>(1)/projected.at<double>(3),
                      projected.at<double>(2)/projected.at<double>(3));
  }



}

void StereoCamera::ReprojectTo3D(const cv::Mat &disparity_image, cv::Mat &point_cloud, const std::vector<cv::Vec2i> &connected_region) const {

  if(point_cloud.data == 0x0) point_cloud.create(disparity_image.size(),CV_32FC3);
  cv::Mat disp_image = disparity_image.clone();

  //mask point cloud if required
  cv::Mat mask;
  if(connected_region.size() == 0){
    mask = cv::Mat::ones(disparity_image.size(),CV_8UC1) * 255;
  }else{
    mask = cv::Mat::zeros(disparity_image.size(),CV_8UC1);
  }
  unsigned char *mask_data = (unsigned char *)mask.data;
  const int cols = mask.cols;
  for(size_t i=0;i<connected_region.size();i++){
    const cv::Vec2i &pixel = connected_region[i];
    mask_data[pixel[1]*cols + pixel[0]] = 255;
  }

 

  TestReproject(disp_image,point_cloud,reprojection_matrix_);


  for (int r = 0; r < point_cloud.rows; r++){
    for (int c =0; c < point_cloud.cols; c++ ){
      
        
      if(mask.at<unsigned char>(r,c) != 255)
        point_cloud.at<cv::Vec3d>(r,c) = cv::Vec3d(0,0,0) ;      
      
      if(point_cloud.at<cv::Vec3d>(r,c)[2] < 0 || point_cloud.at<cv::Vec3d>(r,c)[2] == 10000)
        point_cloud.at<cv::Vec3d>(r,c) = cv::Vec3d(0,0,0);
 
      cv::Point3d point(point_cloud.at<cv::Vec3d>(r,c) );
      if(point != cv::Point3d(0,0,0)){
 
        
      }
     
    }
  }
  
  //cv::imwrite("negdisp.png",z);
  //ofs.close();


}

void StereoCamera::Rectify(const cv::Size image_size) {

  cv::Mat inverse_ext;// = extrinsic_matrix_.clone();
  cv::invert(extrinsic_matrix_,inverse_ext);

  cv::stereoRectify(left_eye_->intrinsic_matrix_,left_eye_->distortion_params_,
                    right_eye_->intrinsic_matrix_,right_eye_->distortion_params_,
                    image_size,
                    inverse_ext(cv::Range(0,3),cv::Range(0,3)),
                    inverse_ext(cv::Range(0,3),cv::Range(3,4)),
                    /*
                    extrinsic_matrix_(cv::Range(0,3),cv::Range(0,3)),
                    extrinsic_matrix_(cv::Range(0,3),cv::Range(3,4)),
                    */
                    R1, R2, P1, P2, reprojection_matrix_,
                    0,//CV_CALIB_ZERO_DISPARITY, // 0 || CV_CALIB_ZERO_DISPARITY
                    0,  // -1 = default scaling, 0 = no black pixels, 1 = no source pixels lost
                    cv::Size(), &roi1, &roi2); 

  InitRectified();

  //store ROI1/2 in the stereo image class and then write method to extract these roi's whenever
  //useful image area methods are needed
  cv::initUndistortRectifyMap(left_eye_->intrinsic_matrix_,
      left_eye_->distortion_params_,
      R1,P1,image_size,CV_32F,mapx_left_,mapy_left_); //must be 16s or 32f

  cv::initUndistortRectifyMap(right_eye_->intrinsic_matrix_,
      right_eye_->distortion_params_,
      R2,P2,image_size,CV_32F,mapx_right_,mapy_right_);


  rectified_ = true;

}

void StereoCamera::RemapLeftFrame(cv::Mat &image) const {

  cv::Mat rectified;
  cv::remap(image,rectified,mapx_left_,mapy_left_,CV_INTER_CUBIC);
  rectified.copyTo(image);
}

void StereoCamera::RemapRightFrame(cv::Mat &image) const {

  cv::Mat rectified;
  cv::remap(image,rectified,mapx_right_,mapy_right_,CV_INTER_CUBIC);
  rectified.copyTo(image);
  
}
