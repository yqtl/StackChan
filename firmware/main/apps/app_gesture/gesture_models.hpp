#pragma once

#include <hand_detect.hpp>
#include <hand_gesture_recognition.hpp>

// The upstream wrappers call Model::run() with its single-core default.
// Keep their models, preprocessing and postprocessing, but let ESP-DL split
// expensive operators across the S3's cores. Only the Gesture app opts in.
class GestureDetector : public hand_detect::ESPDet {
public:
    GestureDetector()
        : ESPDet("espdet_pico_224_224_hand.espdl", default_score_thr, default_nms_thr) {}

    std::list<dl::detect::result_t> &run(const dl::image::img_t &image) override
    {
        m_image_preprocessor->preprocess(image);
        m_model->run(dl::RUNTIME_MODE_AUTO);
        m_postprocessor->clear_result();
        m_postprocessor->postprocess();
        return m_postprocessor->get_result(image.width, image.height);
    }
};

class GestureClassifier : public hand_gesture_recognition::MobileNetV2 {
public:
    GestureClassifier()
        : MobileNetV2("mobilenetv2_0_5_128_128_gesture.espdl", default_topk, default_score_thr) {}

    std::vector<dl::cls::result_t> run_crop(const dl::image::img_t &image,
                                         const std::vector<int> &box)
    {
        m_image_preprocessor->preprocess(image, box);
        m_model->run(dl::RUNTIME_MODE_AUTO);
        return m_postprocessor->postprocess();
    }
};
