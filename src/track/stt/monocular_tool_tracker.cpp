#include "../../../headers/track/stt/monocular_tool_tracker.hpp"
#include "../../../headers/track/pwp3d/mono_pwp3d.hpp"

using namespace ttrk;

MonocularToolTracker::MonocularToolTracker(const float radius, const float height, const std::string &config_dir, const std::string &calibration_filename):SurgicalToolTracker(radius,height),camera_(new MonocularCamera(config_dir + "/" + calibration_filename)){
  localizer_.reset(new MonoPWP3D(config_dir,camera_));
  //boost::shared_ptr<MonoPWP3D> mono_pwp3d = boost::dynamic_pointer_cast<MonoPWP3D>(localizer_);
  //mono_pwp3d->Camera() = camera_;
}

bool MonocularToolTracker::Init(){

  std::vector<std::vector<cv::Vec2i> >connected_regions;
  if(!FindConnectedRegions(frame_->GetClassificationMap(),connected_regions)) return false;

  for(auto connected_region = connected_regions.cbegin(); connected_region != connected_regions.end(); connected_region++){

    KalmanTracker new_tracker(boost::shared_ptr<Model>(new  MISTool(radius_,height_) ));
    //new_tracker.model_.reset( new MISTool(radius_,height_) );

    tracked_models_.push_back( new_tracker ); 

    Init2DPoseFromMOITensor(*connected_region,tracked_models_.back());

  }

  return true;

}


void MonocularToolTracker::Init2DPoseFromMOITensor(const std::vector<cv::Vec2i> &connected_region, KalmanTracker &tracked_model){

  const cv::Vec2i center_of_mass = FindCenterOfMass(connected_region);
  cv::Mat moi_tensor = cv::Mat::zeros(2,2,CV_32FC1);
  float *data = (float *)moi_tensor.data;
  
  /* moi [ xx , xy ; yx , yy ] */
  for(int r=0;r<2;r++){
    for(int c=0;c<2;c++){
      if(r==1 && c== 0) {
	      //symmetric...
	      data[r*2 + c] = data[c*2 + r];
	      continue;
      }
      
      for(size_t i=0;i<connected_region.size();i++){

        cv::Vec2i p = connected_region[i] - center_of_mass;
        int p_i = p[0]*(1-r) + p[1]*r;
        int p_j = p[0]*(1-c) + p[1]*c;

        data[r*2 + c] +=  (( (p[0]*p[0]+p[1]*p[1])*(r==c)) 
          - (p_i*p_j) );
      }
    }
  }

  cv::Mat eigenvals = cv::Mat::zeros(2,1,CV_32FC1);
  cv::Mat eigenvecs = cv::Mat::zeros(2,2,CV_32FC1);
  cv::eigen(moi_tensor,eigenvals,eigenvecs);

  float *e = (float *)eigenvecs.data;
  float *v = (float *)eigenvals.data;
  
  cv::Vec2f central_axis(e[2],e[3]);
  cv::Vec2f horizontal_axis(e[0],e[1]);

  CheckCentralAxisDirection(center_of_mass,central_axis);
  
  cv::Vec2f normed_central_axis,normed_horizontal_axis;
  cv::normalize(central_axis,normed_central_axis);
  cv::normalize(horizontal_axis,normed_horizontal_axis);
  central_axis = normed_central_axis;
  horizontal_axis = normed_horizontal_axis; 

  if(v[1] < v[0]){
    float tmp = v[1];
    v[1] = v[0];
    v[0] = tmp;
  }

  const float radius = sqrt( (2.0*std::abs(v[0]))/connected_region.size() ); 
  const float length = sqrt( ((12.0*std::abs(v[1])) / connected_region.size())  - 3*radius*radius);

  cv::Vec2f point = cv::Vec2f(center_of_mass) + 0.5*length*central_axis;
  cv::Vec2f top = cv::Vec2f(center_of_mass) + (radius)*horizontal_axis;
  cv::Vec2f bottom = cv::Vec2f(center_of_mass) - (radius)*horizontal_axis;

  cv::Point3f top_unp = camera_->UnProjectPoint(cv::Point2i(top));
  cv::Point3f bottom_unp = camera_->UnProjectPoint(cv::Point2i(bottom));
  cv::Point3f center_unp = camera_->UnProjectPoint(cv::Point2i(center_of_mass));
  cv::Vec3f diff = cv::Vec3f(top_unp) - cv::Vec3f(bottom_unp);
  float abs_diff = sqrt( static_cast<double>( diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2] ) );

  
  float z = (2*tracked_models_.back().PtrToModel()->Radius())/abs_diff;
  
  cv::Vec3f unp_point = cv::Vec3f(camera_->UnProjectPoint(cv::Point2f(point)));
  cv::Vec3f center_of_mass_3d = cv::Vec3f(camera_->UnProjectPoint(cv::Point2f(center_of_mass)));

  cv::Vec3f central_axis_3d = (z*unp_point) - (z*center_of_mass_3d);
  center_of_mass_3d = center_of_mass_3d * z;
  
  /*
  float width = ComputeWidth(v[0],v[1],connected_region.size());

  cv::Vec2f central_axis(e[2],e[3]);
  cv::Vec2f horizontal_axis(e[0],e[1]);

  CheckCentralAxisDirection(center_of_mass,central_axis);
  cv::Vec2f top = cv::Vec2f(center_of_mass) + (width)*horizontal_axis;
  cv::Vec2f bottom = cv::Vec2f(center_of_mass) - (width)*horizontal_axis;

  cv::Point3f top_unp = camera_->UnProjectPoint(cv::Point2i(top));
  cv::Point3f bottom_unp = camera_->UnProjectPoint(cv::Point2i(bottom));
  cv::Point3f center_unp = camera_->UnProjectPoint(cv::Point2i(center_of_mass));
  cv::Vec3f diff = cv::Vec3f(top_unp) - cv::Vec3f(bottom_unp);
  float abs_diff = sqrt( static_cast<double>( diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2] ) );
  
  float z = 2*tracked_model.PtrToModel()->Radius()/abs_diff;

  cv::Vec3f center = z*cv::Vec3f(center_unp);
  cv::Vec3f end = z*cv::Vec3f(camera_->UnProjectPoint(cv::Point2i(cv::Vec2f(center_of_mass) + (10*central_axis))));
  cv::Vec3f central_axis_3d = end - center;

  cv::Mat whatv = cv::Mat::zeros(cv::Size(614,524),CV_8UC1);
  cv::line(whatv,cv::Point(camera_->ProjectPointToPixel(end)),cv::Point(camera_->ProjectPointToPixel(center)),cv::Scalar(255),2);
  cv::imwrite("aghoa.png",whatv);
  */

  cv::line(frame_->GetImageROI(),cv::Point(camera_->ProjectPointToPixel(cv::Point3f(center_of_mass_3d))),cv::Point(camera_->ProjectPointToPixel(cv::Point3f(unp_point))),cv::Scalar(241,243,12),4);
  cv::line(frame_->GetImageROI(),cv::Point(camera_->ProjectPointToPixel(cv::Point3f(top_unp))),cv::Point(camera_->ProjectPointToPixel(cv::Point3f(bottom_unp))),cv::Scalar(241,243,12),4); 

  cv::imwrite("testiamgething.png",frame_->GetImageROI());

  tracked_model.SetPose(center_of_mass_3d,central_axis_3d);
} 

float MonocularToolTracker::ComputeWidth(float e1, float e2, size_t mass) const {
  float i_x = std::abs(e1);
  float i_y = std::abs(e2);
  
  if(i_x < i_y)
    std::swap(i_x,i_y);
  
  return (float)sqrt( (2.0*i_y) / mass );

}

const cv::Vec2i MonocularToolTracker::FindCenterOfMass(const std::vector<cv::Vec2i> &connected_region) const {

  cv::Vec2f com(0,0);

  for(auto pt = connected_region.begin() ; pt != connected_region.end() ; pt++ ){

    com += *pt;

  }
  
  com[0] = com[0]/connected_region.size();
  com[1] = com[1]/connected_region.size();

  return com;

}

void MonocularToolTracker::DrawModelOnFrame(const KalmanTracker &tracked_model, cv::Mat canvas) const {

std::vector<SimplePoint<> > transformed_points = tracked_model.ModelPointsAtCurrentPose();
  for(auto point = transformed_points.begin(); point != transformed_points.end(); point++ ){

    cv::Vec2f projected = camera_->ProjectPoint(point->vertex_);

    for(auto neighbour_index = point->neighbours_.begin(); neighbour_index != point->neighbours_.end(); neighbour_index++){
      
      const SimplePoint<> &neighbour = transformed_points[*neighbour_index];
      cv::Vec2f projected_neighbour = camera_->ProjectPoint( neighbour.vertex_ );

      if(canvas.channels() == 3)
        line(canvas,cv::Point2f(projected),cv::Point2f(projected_neighbour),cv::Scalar(255,0,255),1,CV_AA);
      if(canvas.channels() == 1)
        line(canvas,cv::Point2f(projected),cv::Point2f(projected_neighbour),(unsigned char)255,1,CV_AA);
    }
  }
}





