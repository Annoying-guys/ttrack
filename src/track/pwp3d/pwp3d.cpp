#include <cinder/ImageIo.h>
#include <cinder/app/AppBasic.h>
#include <cinder/Camera.h>
#include <cinder/gl/Light.h>

#include <boost/math/special_functions/fpclassify.hpp>
#include <ctime>
#include <CinderOpenCV.h>
#include <cinder/CinderMath.h>

#include "../../../include/track/pwp3d/pwp3d.hpp"
#include "../../../include/utils/helpers.hpp"
#include "../../../include/resources.hpp"
#include "../../../include/constants.hpp"

using namespace ttrk;

PWP3D::PWP3D(const int width, const int height) : heaviside_width_(3,1,3,40,"Heaviside Width"){

  //need the colour buffer to be 32bit
  ci::gl::Fbo::Format format;
  format.setColorInternalFormat(GL_RGBA32F);
  format.enableColorBuffer(true, 1);
  front_depth_framebuffer_ = ci::gl::Fbo(width, height, format);
 
  //need 2 colour buffers for back contour
  format.enableColorBuffer(true, 2);
  back_depth_framebuffer_ = ci::gl::Fbo(width, height, format);

  HEAVYSIDE_WIDTH = 6;

  NUM_STEPS = 45;// 125;

  curr_step = NUM_STEPS; //so we report converged and ask for a new frame at the start

}

PWP3D::~PWP3D(){

  front_depth_framebuffer_.getDepthTexture().setDoNotDispose(false);
  front_depth_framebuffer_.getTexture().setDoNotDispose(false);
  front_depth_framebuffer_.reset();

  back_depth_framebuffer_.getDepthTexture().setDoNotDispose(false);
  back_depth_framebuffer_.getTexture(0).setDoNotDispose(false);
  back_depth_framebuffer_.getTexture(1).setDoNotDispose(false);
  back_depth_framebuffer_.reset();

  //front_depth_.reset();
  //back_depth_and_contour_.reset();

}

void PWP3D::LoadShaders(){

  front_depth_ = ci::gl::GlslProg(ci::app::loadResource(PWP3D_FRONT_DEPTH_VERT), ci::app::loadResource(PWP3D_FRONT_DEPTH_FRAG));
  back_depth_and_contour_ = ci::gl::GlslProg(ci::app::loadResource(PWP3D_BACK_DEPTH_AND_CONTOUR_VERT), ci::app::loadResource(PWP3D_BACK_DEPTH_AND_CONTOUR_FRAG));

}

void PWP3D::ComputeAreas(cv::Mat &sdf, size_t &fg_area, size_t &bg_area, size_t &contour_area){

  fg_area = bg_area = 0;

  for (auto r = 0; r < sdf.rows; ++r){
    for (auto c = 0; c < sdf.rows; ++c){

      if (sdf.at<float>(r, c) <= float(3) - 1e-1 && sdf.at<float>(r, c) >= -float(3) + 1e-1)
        contour_area++;

      fg_area += sdf.at<float>(r, c);
      bg_area += (1.0f - sdf.at<float>(r, c));
    }
  }

}

void PWP3D::UpdateJacobian(const float region_agreement, const float sdf, const float dsdf_dx, const float dsdf_dy, const float fx, const float fy, const cv::Vec3f &front_intersection_point, const cv::Vec3f &back_intersection_point, const boost::shared_ptr<const Model> model, cv::Matx<float, 1, 7> &jacobian){

  const float z_inv_sq_front = 1.0f / (front_intersection_point[2] * front_intersection_point[2]);
  const float z_inv_sq_back = 1.0f / (back_intersection_point[2] * back_intersection_point[2]);

  //compute the derivatives
  std::vector<ci::Vec3f> front_jacs = model->ComputeJacobian(front_intersection_point, 0);
  std::vector<ci::Vec3f> back_jacs = model->ComputeJacobian(back_intersection_point, 0);

  //for each degree of freedom, compute the jacobian update
  for (size_t dof = 0; dof < model->GetBasePose().GetNumDofs(); ++dof){

    if (dof >= 7)
      throw std::runtime_error("");

    const ci::Vec3f &dof_derivatives_front = front_jacs[dof];
    const ci::Vec3f &dof_derivatives_back = back_jacs[dof];

    if (sdf == 0.0f){
      const float deriv_x_front = (front_intersection_point[2] * dof_derivatives_front[0]) - (front_intersection_point[0] * dof_derivatives_front[2]);
      const float deriv_x = dsdf_dx * (fx * z_inv_sq_front * deriv_x_front);

      const float deriv_y_front = (front_intersection_point[2] * dof_derivatives_front[1]) - (front_intersection_point[1] * dof_derivatives_front[2]);
      const float deriv_y = dsdf_dy * (fy * z_inv_sq_front * deriv_y_front);

      const float pval = DeltaFunction(sdf) * (deriv_x + deriv_y);

      jacobian(dof) = region_agreement * pval;

    }
    else{
      //actually compute the cost function equation for the degree of freedom in question

      const float deriv_x_front = (front_intersection_point[2] * dof_derivatives_front[0]) - (front_intersection_point[0] * dof_derivatives_front[2]);
      const float deriv_x_back = (back_intersection_point[2] * dof_derivatives_back[0]) - (back_intersection_point[0] * dof_derivatives_back[2]);
      const float deriv_x = dsdf_dx * ((fx * z_inv_sq_front * deriv_x_front) + (fx * z_inv_sq_back * deriv_x_back));

      const float deriv_y_front = (front_intersection_point[2] * dof_derivatives_front[1]) - (front_intersection_point[1] * dof_derivatives_front[2]);
      const float deriv_y_back = (back_intersection_point[2] * dof_derivatives_back[1]) - (back_intersection_point[1] * dof_derivatives_back[2]);
      const float deriv_y = dsdf_dy * ((fy * z_inv_sq_front * deriv_y_front) + (fy * z_inv_sq_back * deriv_y_back));

      //pval += dsdf_dy * (fy * (z_inv_sq_front*((front_intersection_point[2] * dof_derivatives_front[1]) - (front_intersection_point[1] * dof_derivatives_front[2]))) + fy * (z_inv_sq_back*((back_intersection_point[2] * dof_derivatives_back[1]) - (back_intersection_point[1] * dof_derivatives_back[2]))));
      const float pval = DeltaFunction(sdf) * (deriv_x + deriv_y);

      jacobian(dof) = region_agreement * pval;
    }

  }

}

bool PWP3D::FindClosestIntersection(const float *sdf_im, const int r, const int c, const int height, const int width, int &closest_r, int &closest_c) const {
  
  const float &sdf_val = sdf_im[r * width + c];
  const int ceil_sdf = (int)std::abs(ceil(sdf_val));
  for (int w_c = c - ceil_sdf; w_c <= c + ceil_sdf; ++w_c){
    
    const int up_idx = (r + ceil_sdf)*width + w_c;
    const int down_idx = (r - ceil_sdf)*width + w_c;
    if (sdf_im[up_idx] >= 0.0){
      closest_r = r + ceil_sdf;
      closest_c = w_c;
      return true;
    }
    else if (sdf_im[down_idx] >= 0.0){
      closest_r = r - ceil_sdf;
      closest_c = w_c;
      return true;
    }
  }

  for (int w_r = r - ceil_sdf; w_r <= r + ceil_sdf; ++w_r){

    const int left_idx = w_r*width + c - ceil_sdf;
    const int right_idx = w_r*width + c + ceil_sdf;
    if (sdf_im[left_idx] >= 0.0){
      closest_r = w_r;
      closest_c = c - ceil_sdf;
      return true;
    }
    else if (sdf_im[right_idx] >= 0.0){
      closest_r = w_r;
      closest_c = c + ceil_sdf;
      return true;
    }
  }
  
  return false;

}

float PWP3D::GetRegionAgreement(const cv::Mat &classification_image, const int r, const int c, const float sdf) const {
  
  const float heaviside_value = HeavisideFunction(sdf);

  const float Pf = classification_image.at<cv::Vec4f>(r, c)[1]; //returns foreground pixel likelihood
  const float Pb = classification_image.at<cv::Vec4f>(r, c)[0]; //returns background pixel likelihood

  return (Pf - Pb) / ((heaviside_value*Pf) + ((1 - heaviside_value)*Pb));

}


float PWP3D::GetErrorValue(const cv::Mat &classification_image, const int row_idx, const int col_idx, const float sdf_value, const int target_label) const{

  const float Pf = classification_image.at<cv::Vec4f>(row_idx, col_idx)[1];
  const float Pb = classification_image.at<cv::Vec4f>(row_idx, col_idx)[0];
  //assert(pixel_probability >= 0.0f && pixel_probability <= 1.0f);

  const float heaviside_value = HeavisideFunction(sdf_value);

  float v = (heaviside_value * Pf) + ((1 - heaviside_value)*(Pb));
  v += 0.0000001f;
  return -log(v);

}

void PWP3D::RenderModelForDepthAndContour(const boost::shared_ptr<Model> mesh, const boost::shared_ptr<MonocularCamera> camera, cv::Mat &front_depth, cv::Mat &back_depth, cv::Mat &contour){

  assert(front_depth_framebuffer_.getWidth() == camera->Width() && front_depth_framebuffer_.getHeight() == camera->Height());

  //setup camera/transform/matrices etc
  ci::gl::pushMatrices();

  camera->SetupCameraForDrawing();

  ci::gl::enableDepthWrite();
  ci::gl::enableDepthRead();
  glDisable(GL_LIGHTING);

  //// Render front depth 
  front_depth_framebuffer_.bindFramebuffer();
  glClearColor((float)GL_FAR, (float)GL_FAR, (float)GL_FAR, (float)GL_FAR);

  glClearDepth(1.0f);
  glDepthFunc(GL_LESS);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  ////bind the front depth shader and render
  front_depth_.bind();
  mesh->RenderMaterial();
  front_depth_.unbind();
 
  front_depth_framebuffer_.unbindFramebuffer();
  glFinish();

  //// Render back depth + contour 
  back_depth_framebuffer_.bindFramebuffer();
  glClearColor((float)GL_FAR, (float)GL_FAR, (float)GL_FAR, (float)GL_FAR);

  glClearDepth(0.0f);
  glDepthFunc(GL_GREATER);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  back_depth_and_contour_.bind();
  back_depth_and_contour_.uniform("tex_w", float(back_depth_framebuffer_.getWidth()));
  back_depth_and_contour_.uniform("tex_h", float(back_depth_framebuffer_.getHeight()));
  back_depth_and_contour_.uniform("far", float(GL_FAR));
  
  ci::gl::Texture tex_fd = front_depth_framebuffer_.getTexture(0);
  tex_fd.enableAndBind();
  back_depth_and_contour_.uniform("tex_fd", 0); //bind it to the current texture
  //back_depth_and_contour_.uniform("tex_cols", 1);
  mesh->RenderMaterial();
  tex_fd.disable();
  tex_fd.unbind();
  back_depth_and_contour_.unbind();

  back_depth_framebuffer_.unbindFramebuffer();

  //Bring depth test back to normal mode
  glClearDepth(1.0f);
  glDepthFunc(GL_LESS);

  //neccessary to get the results NOW.
  glFinish();

  ci::gl::popMatrices();

  camera->ShutDownCameraAfterDrawing();

  front_depth_framebuffer_.getTexture();
  back_depth_framebuffer_.getTexture();
  back_depth_framebuffer_.getTexture(1);

  cv::Mat front_depth_flipped = ci::toOcv(front_depth_framebuffer_.getTexture());
  cv::Mat back_depth_flipped = ci::toOcv(back_depth_framebuffer_.getTexture(0));
  cv::Mat mcontour = ci::toOcv(back_depth_framebuffer_.getTexture(1));
  cv::Mat fmcontour;
  cv::flip(front_depth_flipped, front_depth, 0);
  cv::flip(back_depth_flipped, back_depth, 0);
  cv::flip(mcontour, fmcontour, 0);

  contour = cv::Mat(mcontour.size(), CV_8UC1);
  float *src = (float *)fmcontour.data;
  unsigned char *dst = (unsigned char*)contour.data;

  for (int r = 0; r < mcontour.rows; ++r){
    for (int c = 0; c < mcontour.cols; ++c){
      dst[r * mcontour.cols + c] = (unsigned char)src[(r * mcontour.cols + c)*4];
    }
  }

}

void PWP3D::ProcessSDFAndIntersectionImage(const boost::shared_ptr<Model> mesh, const boost::shared_ptr<MonocularCamera> camera, cv::Mat &sdf_image, cv::Mat &front_intersection_image, cv::Mat &back_intersection_image) {

  //find all the pixels which project to intersection points on the model
  sdf_image = cv::Mat(frame_->GetImageROI().size(),CV_32FC1);
  front_intersection_image = cv::Mat::zeros(frame_->GetImageROI().size(),CV_32FC3);
  back_intersection_image = cv::Mat::zeros(frame_->GetImageROI().size(),CV_32FC3);

  cv::Mat front_depth, back_depth, contour;
  RenderModelForDepthAndContour(mesh, camera, front_depth, back_depth, contour );
  
  cv::Mat unprojected_image_plane = camera->GetUnprojectedImagePlane(front_intersection_image.cols, front_intersection_image.rows);

  for (int r = 0; r < front_intersection_image.rows; r++){
    for (int c = 0; c < front_intersection_image.cols; c++){

      if (std::abs(front_depth.at<cv::Vec4f>(r, c)[0] - GL_FAR) > EPS){
        const cv::Vec2f &unprojected_pixel = unprojected_image_plane.at<cv::Vec2f>(r, c);
        front_intersection_image.at<cv::Vec3f>(r, c) = front_depth.at<cv::Vec4f>(r, c)[0]*cv::Vec3f(unprojected_pixel[0], unprojected_pixel[1], 1);
      }
      else{
        front_intersection_image.at<cv::Vec3f>(r, c) = cv::Vec3f((int)GL_FAR, (int)GL_FAR, (int)GL_FAR);
      }
      if (std::abs(back_depth.at<cv::Vec4f>(r, c)[0] - GL_FAR) > EPS){
        const cv::Vec2f &unprojected_pixel = unprojected_image_plane.at<cv::Vec2f>(r, c);
        back_intersection_image.at<cv::Vec3f>(r, c) = back_depth.at<cv::Vec4f>(r, c)[0]*cv::Vec3f(unprojected_pixel[0], unprojected_pixel[1], 1);
      }
      else{
        back_intersection_image.at<cv::Vec3f>(r, c) = cv::Vec3f((int)GL_FAR, (int)GL_FAR, (int)GL_FAR);
      }

    }
  }


  distanceTransform(contour, sdf_image, CV_DIST_L2, CV_DIST_MASK_PRECISE);

  //flip the sign of the distance image for outside pixels
  for(int r=0;r<sdf_image.rows;r++){
    for(int c=0;c<sdf_image.cols;c++){
      if (std::abs(front_depth.at<cv::Vec4f>(r, c)[0] - GL_FAR) < EPS)
        sdf_image.at<float>(r,c) *= -1;
    }
  }
  
}


