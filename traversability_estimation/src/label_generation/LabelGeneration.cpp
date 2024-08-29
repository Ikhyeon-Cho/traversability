/*
 * LabelGeneration.cpp
 *
 *  Created on: Aug 17, 2023
 *      Author: Ikhyeon Cho
 *	 Institute: Korea Univ. ISR (Intelligent Systems & Robotics) Lab
 *       Email: tre0430@korea.ac.kr
 */

#include "label_generation/LabelGeneration.h"

void LabelGeneration::terrainFeatureCallback(const grid_map_msgs::GridMapConstPtr& msg)
{
  // Allocate featuremap memory
  if (!featuremap_ptr_)
  {
    auto length_x = msg->info.length_x;
    auto length_y = msg->info.length_y;
    auto resolution = msg->info.resolution;
    featuremap_ptr_ = std::make_shared<grid_map::HeightMap>(length_x, length_y, resolution);
  }

  grid_map::GridMapRosConverter::fromMessage(*msg, *featuremap_ptr_);
  const auto& featuremap = *featuremap_ptr_;

  // Initialize labelmap geometry from featuremap
  if (!is_labelmap_initialized_)
  {
    initializeLabelMap(featuremap);
    is_labelmap_initialized_ = true;
  }

  updateLabelmapFrom(featuremap);

  recordFootprints();

  visualizeLabelSubmap(grid_map::Length(5, 5));

  visualizeLabelMap();
}

void LabelGeneration::updateLabelmapFrom(const grid_map::HeightMap& featuremap)
{
  bool param_vis = pnh_.param<bool>("recordNegativeLabel", false);

  // Update labelmap with featuremap
  for (grid_map::GridMapIterator it(featuremap); !it.isPastEnd(); ++it)
  {
    const auto& featuremap_iterator = *it;
    if (featuremap.isEmptyAt(featuremap_iterator))
      continue;

    grid_map::Position grid_position;
    featuremap.getPosition(featuremap_iterator, grid_position);

    grid_map::Index labelmap_index;
    if (!labelmap_.getIndex(grid_position, labelmap_index))
      continue;

    if (labelmap_.at(labelmap_.getVarianceLayer(), labelmap_index) > 0.03)
      continue;

    if (!labelmap_.isEmptyAt("footprint", labelmap_index))
      continue;

    const auto& height = featuremap.at(featuremap.getHeightLayer(), featuremap_iterator);
    const auto& step = featuremap.at("step", featuremap_iterator);
    const auto& slope = featuremap.at("slope", featuremap_iterator);
    const auto& roughness = featuremap.at("roughness", featuremap_iterator);
    const auto& curvature = featuremap.at("curvature", featuremap_iterator);
    const auto& variance = featuremap.at(featuremap.getVarianceLayer(), featuremap_iterator);

    labelmap_.at(labelmap_.getHeightLayer(), labelmap_index) = height;
    labelmap_.at("step", labelmap_index) = step;
    labelmap_.at("slope", labelmap_index) = slope;
    labelmap_.at("roughness", labelmap_index) = roughness;
    labelmap_.at("curvature", labelmap_index) = curvature;
    labelmap_.at(labelmap_.getVarianceLayer(), labelmap_index) = variance;

    valid_indices_.insert(labelmap_index);

    if (param_vis && step > max_acceptable_step_)
    {
      labelmap_.at("traversability_label", labelmap_index) = (float)Traversability::NON_TRAVERSABLE;
    }
  }
}

void LabelGeneration::initializeLabelMap(const grid_map::HeightMap& featuremap)
{
  // Define grid resolution
  auto resolution = featuremap.getResolution();
  // labelmap_ = std::make_shared<grid_map::HeightMap>(map_length.x(), map_length.y(), resolution);
  labelmap_.setGeometry(labelmap_.getLength(), resolution);
  labelmap_.setFrameId(featuremap.getFrameId());

  // Add layers
  labelmap_.addLayer("step");
  labelmap_.addLayer("slope");
  labelmap_.addLayer("roughness");
  labelmap_.addLayer("curvature");
  labelmap_.addLayer("footprint");
  labelmap_.addLayer("traversability_label");
  labelmap_.setBasicLayers({ labelmap_.getHeightLayer(), labelmap_.getVarianceLayer() });

  valid_indices_.reserve(labelmap_.getSize().prod());
}

void LabelGeneration::visualizeLabelSubmap(const grid_map::Length& length)
{
  // get current robot pose
  auto [get_transform_b2m, base2map] = tf_.getTransform(baselink_frame, map_frame);
  if (!get_transform_b2m)
    return;

  auto robot_position = grid_map::Position(base2map.transform.translation.x, base2map.transform.translation.y);

  // Visualize submap
  bool get_submap{ false };
  auto submap = labelmap_.getSubmap(robot_position, length, get_submap);
  if (!get_submap)
    return;

  grid_map_msgs::GridMap message;
  grid_map::GridMapRosConverter::toMessage(submap, message);
  pub_labelmap_local_.publish(message);
}

void LabelGeneration::visualizeLabelMap()
{
  // Visualize global labelmap
  sensor_msgs::PointCloud2 cloud_msg;
  toPointCloud2(labelmap_, labelmap_.getLayers(), valid_indices_, cloud_msg);
  pub_labelmap_global_.publish(cloud_msg);

  // Visualize label map region
  visualization_msgs::Marker msg_map_region;
  HeightMapMsgs::toMapRegion(labelmap_, msg_map_region);
  pub_labelmap_region_.publish(msg_map_region);
}

void LabelGeneration::generateTraversabilityLabels()
{
  // recordNegativeLabel();

  recordFootprints();

  // recordUnknownAreas();
}

void LabelGeneration::recordFootprints()
{
  // Get Transform from base_link to map (typically provided by 3D pose estimator)
  auto [get_transform_b2m, base2map] = tf_.getTransform(baselink_frame, map_frame);
  if (!get_transform_b2m)
    return;

  grid_map::Position footprint(base2map.transform.translation.x, base2map.transform.translation.y);
  grid_map::CircleIterator iterator(labelmap_, footprint, footprint_radius_);

  for (iterator; !iterator.isPastEnd(); ++iterator)
  {
    Eigen::Vector3d grid_with_elevation;
    if (!labelmap_.getPosition3(labelmap_.getHeightLayer(), *iterator, grid_with_elevation))
      continue;

    // Due to perception error, footprint terrain geometry sometimes become messy
    if (labelmap_.at("step", *iterator) > max_acceptable_step_)
      continue;

    labelmap_.at("traversability_label", *iterator) = (float)Traversability::TRAVERSABLE;
    labelmap_.at("footprint", *iterator) = (float)Traversability::TRAVERSABLE;
  }
}

void LabelGeneration::recordNegativeLabel()
{
  for (grid_map::GridMapIterator iter(labelmap_); !iter.isPastEnd(); ++iter)
  {
    Eigen::Vector3d grid_with_elevation;
    if (!labelmap_.getPosition3(labelmap_.getHeightLayer(), *iter, grid_with_elevation))
      continue;

    if (!std::isfinite(labelmap_.at("step", *iter)))
      continue;

    // Condition for non-traversable areas
    bool non_traversable = labelmap_.at("step", *iter) > max_acceptable_step_;
    if (non_traversable)
    {
      labelmap_.at("traversability_label", *iter) = (float)Traversability::NON_TRAVERSABLE;
      continue;
    }
    else  // Remove noisy labels
    {
      bool label_conflict =
          std::abs(labelmap_.at("traversability_label", *iter) - (float)Traversability::NON_TRAVERSABLE) < 1e-6;
      if (label_conflict)
      {
        labelmap_.at("traversability_label", *iter) = NAN;
      }
    }
  }
}

void LabelGeneration::recordUnknownAreas(grid_map::HeightMap& labelmap)
{
  for (grid_map::GridMapIterator iter(labelmap); !iter.isPastEnd(); ++iter)
  {
    Eigen::Vector3d grid_with_elevation;
    if (!labelmap.getPosition3(labelmap.getHeightLayer(), *iter, grid_with_elevation))
      continue;

    const auto& traversability_label = labelmap.at("traversability_label", *iter);
    bool has_label = std::isfinite(traversability_label);
    if (has_label)
      continue;

    labelmap.at("traversability_label", *iter) = (float)Traversability::UNKNOWN;
  }
}

bool LabelGeneration::saveLabeledData(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
{
  is_labeling_callback_working_ = true;
  std::cout << "Generating Traversability Labels..." << std::endl;

  // See generateTraversabilityLabels callback
  return true;
}

void LabelGeneration::generateTraversabilityLabels(const sensor_msgs::PointCloud2ConstPtr& cloud)
{
  if (!is_labeling_callback_working_)
    return;

  // Load globalmap info
  double map_length_x;
  double map_length_y;
  double grid_resolution;
  if (!ros::param::get("/height_mapping/global_mapping/mapLengthXGlobal", map_length_x))
  {
    ROS_ERROR("[LabelGenerationService] Failed to get global map parameter");
    return;
  }
  ros::param::get("/height_mapping/global_mapping/mapLengthYGlobal", map_length_y);
  ros::param::get("/height_mapping/global_mapping/gridResolution", grid_resolution);

  // Load feature extraction parameter
  double normal_estimation_radius;
  if (!ros::param::get("/feature_extraction/normalEstimationRadius", normal_estimation_radius))
  {
    ROS_ERROR("[LabelGenerationService] Failed to get feature extraction parameter");
    return;
  }

  // Label generation: Non-traversable areas
  recordNegativeLabel();

  // saveLabeledDataToCSV(featuremap);

  // Mark unknown areas as unknown
  // recordUnknownAreas(featuremap);

  // saveUnlabeledDataToCSV(featuremap);

  is_labeling_callback_working_ = false;
}

void LabelGeneration::saveLabeledDataToCSV(const grid_map::HeightMap& labelmap)
{
  std::ofstream file(labeled_data_path_);
  file << "step,slope,roughness,curvature,variance,traversability_label\n";  // header

  // Iterate over the grid map
  for (grid_map::GridMapIterator iter(labelmap); !iter.isPastEnd(); ++iter)
  {
    if (labelmap.isEmptyAt(*iter))
      continue;

    // Write data to csv
    float step = labelmap.at("step", *iter);
    float slope = labelmap.at("slope", *iter);
    float roughness = labelmap.at("roughness", *iter);
    float curvature = labelmap.at("curvature", *iter);
    float variance = labelmap.at("variance", *iter);
    float traversability_label = labelmap.at("traversability_label", *iter);

    if (!std::isfinite(traversability_label))
      continue;

    file << step << "," << slope << "," << roughness << "," << curvature << "," << variance << ","
         << traversability_label << "\n";
  }

  file.close();
}

void LabelGeneration::saveUnlabeledDataToCSV(const grid_map::HeightMap& labelmap)
{
  std::ofstream file(unlabeled_data_path_);
  file << "step,slope,roughness,curvature,variance,traversability_label\n";  // header

  // Iterate over the grid map
  for (grid_map::GridMapIterator iter(labelmap); !iter.isPastEnd(); ++iter)
  {
    if (labelmap.isEmptyAt(*iter))
      continue;

    // Write data to csv
    float step = labelmap.at("step", *iter);
    float slope = labelmap.at("slope", *iter);
    float roughness = labelmap.at("roughness", *iter);
    float curvature = labelmap.at("curvature", *iter);
    float variance = labelmap.at("variance", *iter);
    float traversability_label = labelmap.at("traversability_label", *iter);

    if (traversability_label < (float)Traversability::UNKNOWN - 1e-6)
      continue;

    file << step << "," << slope << "," << roughness << "," << curvature << "," << variance << ","
         << traversability_label << "\n";
  }
  file.close();
}

void LabelGeneration::toPointCloud2(const grid_map::HeightMap& map, const std::vector<std::string>& layers,
                                    const std::unordered_set<grid_map::Index, IndexHash, IndexEqual>& measured_indices,
                                    sensor_msgs::PointCloud2& cloud)
{
  // Header.
  cloud.header.frame_id = map.getFrameId();
  cloud.header.stamp.fromNSec(map.getTimestamp());
  cloud.is_dense = false;

  // Fields.
  std::vector<std::string> fieldNames;
  for (const auto& layer : layers)
  {
    if (layer == map.getHeightLayer())
    {
      fieldNames.push_back("x");
      fieldNames.push_back("y");
      fieldNames.push_back("z");
    }
    else if (layer == "color")
    {
      fieldNames.push_back("rgb");
    }
    else
    {
      fieldNames.push_back(layer);
    }
  }

  cloud.fields.clear();
  cloud.fields.reserve(fieldNames.size());
  int offset = 0;

  for (auto& name : fieldNames)
  {
    sensor_msgs::PointField pointField;
    pointField.name = name;
    pointField.count = 1;
    pointField.datatype = sensor_msgs::PointField::FLOAT32;
    pointField.offset = offset;
    cloud.fields.push_back(pointField);
    offset = offset + pointField.count * 4;  // 4 for sensor_msgs::PointField::FLOAT32
  }                                          // offset value goes from 0, 4, 8, 12, ...

  // Adjusted Resize: Instead of maxNumberOfPoints, use measured_indices.size().
  size_t numberOfMeasuredPoints = measured_indices.size();
  cloud.height = 1;
  cloud.width = numberOfMeasuredPoints;  // Use the size of measured_indices.
  cloud.point_step = offset;
  cloud.row_step = cloud.width * cloud.point_step;
  cloud.data.resize(cloud.height * cloud.row_step);

  // Adjust Points section to iterate over measured_indices.
  std::unordered_map<std::string, sensor_msgs::PointCloud2Iterator<float>> pointFieldIterators;
  for (auto& name : fieldNames)
  {
    pointFieldIterators.insert(std::make_pair(name, sensor_msgs::PointCloud2Iterator<float>(cloud, name)));
  }

  // Iterate over measured_indices instead of using GridMapIterator.
  int count = 0;
  for (const auto& measured_index : measured_indices)
  {
    grid_map::Position3 position;
    if (!map.getPosition3(map.getHeightLayer(), measured_index, position))
      continue;

    const auto& cell_position_x = (float)position.x();
    const auto& cell_position_y = (float)position.y();
    const auto& cell_height = (float)position.z();

    for (auto& pointFieldIterator : pointFieldIterators)
    {
      const auto& pointField = pointFieldIterator.first;
      auto& pointFieldValue = *(pointFieldIterator.second);
      if (pointField == "x")
      {
        pointFieldValue = cell_position_x;
      }
      else if (pointField == "y")
      {
        pointFieldValue = cell_position_y;
      }
      else if (pointField == "z")
      {
        pointFieldValue = cell_height;
      }
      else if (pointField == "rgb")
      {
        pointFieldValue = (float)(map.at("color", measured_index));
      }
      else
      {
        pointFieldValue = (float)(map.at(pointField, measured_index));
      }
    }

    for (auto& pointFieldIterator : pointFieldIterators)
    {
      ++(pointFieldIterator.second);
    }
    ++count;
  }
  cloud.height = 1;
  cloud.width = count;
  cloud.point_step = offset;
  cloud.row_step = cloud.width * cloud.point_step;
  cloud.data.resize(cloud.height * cloud.row_step);
}