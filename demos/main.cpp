//
// Created by fss on 23-1-4.
//

#include <iostream>
#include <opencv2/opencv.hpp>
#include <glog/logging.h>

#include "data/tensor.hpp"
#include "image_util.hpp"
#include "runtime/runtime_ir.hpp"
#include "tick.hpp"

struct Detection {
  cv::Rect box;
  float conf = 0.f;
  int class_id = -1;
};

template<typename T>
T clip(const T &n, const T &lower, const T &upper) {
  return std::max(lower, std::min(n, upper));
}

void ScaleCoords(const cv::Size &img_shape, cv::Rect &coords, const cv::Size &img_origin_shape) {
  float gain = std::min((float) img_shape.height / (float) img_origin_shape.height,
                        (float) img_shape.width / (float) img_origin_shape.width);

  int pad[2] = {(int) (((float) img_shape.width - (float) img_origin_shape.width * gain) / 2.0f),
                (int) (((float) img_shape.height - (float) img_origin_shape.height * gain) / 2.0f)};

  coords.x = (int) std::round(((float) (coords.x - pad[0]) / gain));
  coords.y = (int) std::round(((float) (coords.y - pad[1]) / gain));

  coords.width = (int) std::round(((float) coords.width / gain));
  coords.height = (int) std::round(((float) coords.height / gain));

  coords.x = clip(coords.x, 0, img_origin_shape.width);
  coords.y = clip(coords.y, 0, img_origin_shape.height);
  coords.width = clip(coords.width, 0, img_origin_shape.width);
  coords.height = clip(coords.height, 0, img_origin_shape.height);
}

void SingleImageYoloInfer(const std::string &image_path,
                          const float conf_thresh = 0.25f, const float iou_thresh = 0.25f) {
  assert(!image_path.empty());

  using namespace kuiper_infer;
  const int32_t input_c = 3;
  const int32_t input_h = 640;
  const int32_t input_w = 640;

  RuntimeGraph graph("tmp/yolo/test/yolov5n.pnnx.param",
                     "tmp/yolo/test/yolov5n.pnnx.bin");

  graph.Build("pnnx_input_0", "pnnx_output_0");

  cv::Mat image = cv::imread(image_path);
  assert(!image.empty());
  const int32_t origin_input_h = image.size().height;
  const int32_t origin_input_w = image.size().width;

  cv::Mat out_image;
  Letterbox(image, out_image, {input_h, input_w}, 32, {114, 114, 114}, true);

  cv::Mat rgb_image;
  cv::cvtColor(out_image, rgb_image, cv::COLOR_BGR2RGB);

  cv::Mat normalize_image;
  rgb_image.convertTo(normalize_image, CV_32FC3, 1. / 255.);

  int offset = 0;
  std::vector<cv::Mat> split_images;
  cv::split(normalize_image, split_images);
  assert(split_images.size() == input_c);

  std::shared_ptr<Tensor<float>> input = std::make_shared<Tensor<float>>(input_c, input_h, input_w);
  input->Fill(0.f);
  std::vector<std::shared_ptr<Tensor<float>>> inputs;

  int index = 0;
  for (const auto &split_image : split_images) {
    assert(split_image.total() == input_w * input_h);
    const cv::Mat &split_image_t = split_image.t();
    memcpy(input->at(index).memptr(), split_image_t.data, sizeof(float) * split_image.total());
    index += 1;
    offset += split_image.total();
  }

  inputs.push_back(input);

  TICK(FORWARD)
  std::vector<std::shared_ptr<Tensor<float>>> outputs = graph.Forward(inputs, true);
  TOCK(FORWARD);

  assert(outputs.size() == inputs.size());
  assert(outputs.size() == 1);
  const auto &output = outputs.front();
  const auto &shapes = output->shapes();
  assert(shapes.size() == 3);

  const uint32_t batch = shapes.at(0);
  assert(batch == 1);
  const uint32_t elements = shapes.at(1);
  const uint32_t num_info = shapes.at(2);
  const uint32_t num_classes = shapes.at(2) - 5;
  std::vector<Detection> detections;

  std::vector<cv::Rect> boxes;
  std::vector<float> confs;
  std::vector<int> class_ids;

  const uint32_t b = 0;
  for (uint32_t i = 0; i < elements; ++i) {
    float cls_conf = output->at(b, i, 4);
    if (cls_conf >= conf_thresh) {
      int center_x = (int) (output->at(b, i, 0));
      int center_y = (int) (output->at(b, i, 1));
      int width = (int) (output->at(b, i, 2));
      int height = (int) (output->at(b, i, 3));
      int left = center_x - width / 2;
      int top = center_y - height / 2;

      int best_class_id = -1;
      float best_conf = -1.f;
      for (uint32_t j = 5; j < num_info; ++j) {
        if (output->at(b, i, j) > best_conf) {
          best_conf = output->at(b, i, j);
          best_class_id = int(j - 5);
        }
      }

      boxes.emplace_back(left, top, width, height);
      confs.emplace_back(best_conf * cls_conf);
      class_ids.emplace_back(best_class_id);
    }
  }

  std::vector<int> indices;
  cv::dnn::NMSBoxes(boxes, confs, conf_thresh, iou_thresh, indices);

  for (int idx : indices) {
    Detection det;
    det.box = cv::Rect(boxes[idx]);
    ScaleCoords(cv::Size{input_w, input_h},
                det.box,
                cv::Size{origin_input_w, origin_input_h});

    det.conf = confs[idx];
    det.class_id = class_ids[idx];
    detections.emplace_back(det);
  }

  int font_face = cv::FONT_HERSHEY_COMPLEX;
  double font_scale = 2;

  for (const auto &detection : detections) {
    cv::rectangle(image, detection.box, cv::Scalar(255, 255, 255), 4);
    cv::putText(image, std::to_string(detection.class_id),
                cv::Point(detection.box.x, detection.box.y), font_face, font_scale, cv::Scalar(255, 255, 0), 4);
  }
  cv::imwrite("output.jpg", image);
}

int main() {
  const std::string &image_path = "imgs/bus.jpg";
  SingleImageYoloInfer(image_path);
  return 0;
}