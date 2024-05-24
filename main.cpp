#include <boost/filesystem.hpp>
#include <gst/gst.h>
#include <iostream>
#include <sstream>
#include <string>
#include <list>

using namespace std::literals::string_literals;

class VideoSrc {
public:
    bool init(const boost::filesystem::path &record, const std::string &output_name, std::string *err);

private:
    bool init_(const std::list<boost::filesystem::path> &records, const std::string &output, std::string *err);
    static void on_pad_added(GstElement *element, GstPad *pad, gpointer data);

    struct Pipeline {
        GstElement *pipeline;
        GstElement *sink;
    };

    std::list<Pipeline> pipelines_;
    Pipeline *current_;
};

bool VideoSrc::init(const boost::filesystem::path &record, const std::string &output_name, std::string *err) {
    std::list<boost::filesystem::path> records = {record};
    std::string output = output_name;

    // Simplified pipeline creation
    std::stringstream pipeline_ss;
    pipeline_ss << "filesrc location=" << records.back().string() << " ! "
                << "matroskademux name=demuxer ! "
                << "h264parse ! "
                << "avdec_h264 name=next ! "
                << "appsink name=sink max-buffers=1 sync=FALSE";

    std::string pipeline_str = pipeline_ss.str();

    Pipeline p{nullptr, nullptr};

    GError *gerr = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &gerr);
    if (gerr != nullptr) {
        if (err) *err = "cannot parse input gst pipeline: "s + gerr->message;
        g_error_free(gerr);
        return false;
    }

    GstElement *demuxer = gst_bin_get_by_name(GST_BIN(pipeline), "demuxer");
    GstElement *next = gst_bin_get_by_name(GST_BIN(pipeline), "next");
      GstElement *sink     = nullptr;

    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), next);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstStateChangeReturn ret = gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (err) *err = "cannot run last record to get current caps";
        gst_object_unref(demuxer);
        gst_object_unref(next);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    // get caps
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    p.sink   = sink;
    current_ = &p;

    current_ = nullptr;

    pipelines_.push_back({pipeline, gst_bin_get_by_name(GST_BIN(pipeline), "sink")});
        // current_ = &pipelines_.back();


    gst_object_unref(GST_OBJECT(demuxer));
    gst_object_unref(GST_OBJECT(next));
    gst_object_unref(GST_OBJECT(sink));
    // gst_element_set_state(pipeline, GST_STATE_NULL);
    // gst_object_unref(GST_OBJECT(pipeline));

    return init_(records, output, err);
}

bool VideoSrc::init_(const std::list<boost::filesystem::path> &records, const std::string &output, std::string *err) {
    if (pipelines_.empty()) {
        return true;
    }

    current_ = &pipelines_.front();
    gst_element_set_state(current_->pipeline, GST_STATE_PLAYING);
    GstStateChangeReturn ret = gst_element_get_state(current_->pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        GError *gerr = nullptr;
        gchar *debug_info = nullptr;
        GstMessage *msg = gst_bus_poll(gst_element_get_bus(current_->pipeline), GST_MESSAGE_ERROR, 0);
        gst_message_parse_error(msg, &gerr, &debug_info);
        if (err) *err = "Pipeline error: "s + (gerr ? gerr->message : "Unknown error");
        if (gerr) g_error_free(gerr);
        if (debug_info) g_free(debug_info);
        gst_message_unref(msg);
        return false;
    } else if (ret == GST_STATE_CHANGE_SUCCESS) {
        std::cout << "Pipeline started successfully" << std::endl;
    }



    return true;
}

void VideoSrc::on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    GstElement *next = static_cast<GstElement *>(data);
    GstPad *sinkpad = gst_element_get_static_pad(next, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <video file path>" << std::endl;
        return 1;
    }

    std::string err;
    VideoSrc src;
    if (!src.init(argv[1], "", &err)) {
        std::cerr << "Error initializing video source: " << err << std::endl;
        return 1;
    }

    std::cout << "Video source initialized successfully" << std::endl;
    return 0;
}
