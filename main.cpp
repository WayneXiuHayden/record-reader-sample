#include <boost/filesystem.hpp>
#include <gst/gst.h>
#include <iostream>
#include <sstream>
#include <string>
#include <list>

using namespace std::literals::string_literals;

#define TRY_SET(ptr, val) \
  if (ptr) {              \
    *ptr = val;           \
  }

struct VideoOutput {
  std::string name;
  std::string type;

  struct {
    std::string type;
    std::string format;
    size_t      width;
    size_t      height;

    struct {
      size_t numerator;
      size_t denominator;
    } fps;
  } capabilities;

  std::string path; // only for gst shm

  /**\param val json encoded output item
   */
  bool parseFromJsonString(const std::string &str, std::string *err = nullptr);
  /**\param json_pointer path to video output element. Use '/' as separator,
   * like /outputs/0 - first element in outputs array
   */
  bool parseFromJsonString(const std::string &str,
                           const std::string &json_pointer,
                           std::string       *err = nullptr);
};

class VideoSrc {
public:
  bool init(const boost::filesystem::path &record, const std::string &output_name, std::string *err);

private:
  bool init_(const std::list<boost::filesystem::path> &records, const VideoOutput &output, std::string *err);
  static void on_pad_added(GstElement *element, GstPad *pad, gpointer data);

  /**\return 1 if pipeline was succesfully added, -1 in case of error, 0 if
   * record was rejected without errors
   */
  int addRecord(const boost::filesystem::path &filepath, std::string *err);

  bool caps(std::string *format,
            int *width,
            int *height,
            int *fps_numerator,
            int *fps_denominator,
            std::string *err);

  struct Pipeline
  {
    GstElement *pipeline = nullptr;
    GstElement *sink = nullptr;

    boost::filesystem::path data_filepath;
    std::chrono::nanoseconds begin_ts;
    std::chrono::nanoseconds end_ts; // approximated

    Pipeline(GstElement *pipeline_a,
             GstElement *sink_a,
             const boost::filesystem::path &path,
             const std::chrono::nanoseconds &begin,
             const std::chrono::nanoseconds &end)
        : pipeline{pipeline_a}, sink{sink_a}, data_filepath{path}, begin_ts{begin}, end_ts{end}
    {
    }
  };

  /**\return 1 if pipeline was succesfully created, -1 in case of error, 0 if
   * record was rejected without errors
   * \param retval must be valid pointer
   * \note if retval contains any elements they will not be cleared, but just
   * overwritten
   */
  int makePipeline(const boost::filesystem::path &filepath,
                   Pipeline *retval,
                   std::string *err);

  VideoOutput output_;

  std::list<Pipeline> pipelines_;
  Pipeline *current_;
};

bool VideoSrc::caps(std::string *format,
                    int         *width,
                    int         *height,
                    int         *fps_numerator,
                    int         *fps_denominator,
                    std::string *err) {
  if (current_ == nullptr) {
    TRY_SET(err, "not initialized");
    return false;
  }

  GstPad       *pad         = nullptr;
  GstCaps      *caps        = nullptr;
  GstStructure *caps_struct = nullptr;

  const char *f = nullptr;
  int         w;
  int         h;
  int         n;
  int         d;

  pad = gst_element_get_static_pad(current_->sink, "sink");
  if (pad == nullptr) {
    TRY_SET(err, "can not get pad from appsink");
    goto Failure;
  }

  caps = gst_pad_get_current_caps(pad);
  if (caps == nullptr) {
    TRY_SET(err, "can not get caps from appsink pad");
    goto Failure;
  }

  caps_struct = gst_caps_get_structure(caps, 0);
  if (caps_struct == nullptr) {
    TRY_SET(err, "invalid caps");
    goto Failure;
  }

  if (f = gst_structure_get_string(caps_struct, "format"); f == nullptr) {
    TRY_SET(err, "can not get format from appsink caps");
    goto Failure;
  }

  if (gst_structure_get_int(caps_struct, "width", &w) == false ||
      gst_structure_get_int(caps_struct, "height", &h) == false) {
    TRY_SET(err, "can not get width and height from appsink caps");
    goto Failure;
  }

  if (gst_structure_get_fraction(caps_struct, "framerate", &n, &d) == false) {
    TRY_SET(err, "can not get framerate from appsink caps");
    goto Failure;
  }

  TRY_SET(format, f);
  TRY_SET(width, w);
  TRY_SET(height, h);
  TRY_SET(fps_numerator, n);
  TRY_SET(fps_denominator, d);


  gst_caps_unref(caps);
  gst_object_unref(GST_OBJECT(pad));

  return true;

Failure:
  if (caps) {
    gst_caps_unref(caps);
  }
  if (pad) {
    gst_object_unref(GST_OBJECT(pad));
  }

  return false;
}

bool VideoSrc::init(const boost::filesystem::path &record, const std::string &output_name, std::string *err) {
    std::list<boost::filesystem::path> records = {record};
    VideoOutput output;
    output.name = output_name;

    // std::string output = output_name;

    // Simplified pipeline creation
    std::stringstream pipeline_ss;
    pipeline_ss << "filesrc location=" << records.back().string() << " ! "
                << "matroskademux name=demuxer ! "
                // << "vp9dec name=next ! "
                << "h264parse ! avdec_h264 name=next ! "
                << "appsink name=sink max-buffers=1 sync=FALSE";

    std::string pipeline_str = pipeline_ss.str();

    Pipeline p{nullptr, nullptr, {}, {}, {}};

    GError *gerr = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &gerr);
    if (gerr != nullptr) {
        if (err) *err = "cannot parse input gst pipeline: "s + gerr->message;
        g_error_free(gerr);
        return false;
    }
    std::string format;
    int width;
    int height;
    int numerator;
    int denominator;
    GstElement *demuxer = gst_bin_get_by_name(GST_BIN(pipeline), "demuxer");
    GstElement *next = gst_bin_get_by_name(GST_BIN(pipeline), "next");
    GstElement *sink = nullptr;

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

    if (this->caps(&format, &width, &height, &numerator, &denominator, err) == false) {
      goto Failure;
    };
    // fill VideoOutput structure and use default initializer
    output.capabilities.type = "video/x-raw";
    output.capabilities.format = format;
    output.capabilities.width = width;
    output.capabilities.height = height;
    output.capabilities.fps.numerator = numerator;
    output.capabilities.fps.denominator = denominator;

    // pipelines_.push_back({pipeline, gst_bin_get_by_name(GST_BIN(pipeline), "sink")});
    // current_ = &pipelines_.back();


    gst_object_unref(GST_OBJECT(demuxer));
    gst_object_unref(GST_OBJECT(next));
    gst_object_unref(GST_OBJECT(sink));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));

    current_ = nullptr;

    return init_(records, output, err);

Failure:
  current_ = nullptr;
  if (demuxer) {
    gst_object_unref(GST_OBJECT(demuxer));
  }
  if (next) {
    gst_object_unref(GST_OBJECT(next));
  }
  if (sink) {
    gst_object_unref(GST_OBJECT(sink));
  }
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
  }
  return false;
}

bool VideoSrc::init_(const std::list<boost::filesystem::path> &records, const VideoOutput &output, std::string *err) {
  
  output_ = output;
  if (records.empty()) {
      return true;
  }

  for (const auto &filepath : records) {
      if (addRecord(filepath, err) == false) {
          return false;
      }
  }

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

int VideoSrc::addRecord(const boost::filesystem::path &filepath,
                        std::string                   *err) {
  Pipeline pipeline{nullptr, nullptr, {}, {}, {}};
  switch (makePipeline(filepath, &pipeline, err)) {
  case -1:
    return false;
  case 0:
    return 0;
  case 1:
    break;
  }

  // Stop the pipeline to save resources and avoid state conflicts
  gst_element_set_state(pipeline.pipeline, GST_STATE_NULL);
  
  // add to the list of pipelines
  if (pipelines_.empty()) {
    pipelines_.emplace_back(pipeline);
  } else if (pipeline.begin_ts >= pipelines_.back().begin_ts) {
    pipelines_.emplace_back(pipeline);
  } else {
    for (auto iter = pipelines_.begin(); iter != pipelines_.end(); ++iter) {
      if (pipeline.begin_ts < iter->begin_ts) { // this is the place to insert
        pipelines_.emplace(iter, pipeline);
        break;
      }
    }
  }

  return 1;
}


int VideoSrc::makePipeline(const boost::filesystem::path &filepath,
                           Pipeline                      *retval,
                           std::string                   *err) {
  std::chrono::nanoseconds begin_ts;
  std::chrono::nanoseconds end_ts;
  gint64                   duration_ns = 0;

//   if (match(filepath, output_.name, &begin_ts) == false) {
//     // should never happen at that point
//     return -1;
//   }

  std::stringstream pipeline_ss;
  // clang-format off
  pipeline_ss << "filesrc location=" << filepath.string() << " ! "
              << "matroskademux name=demuxer ! "
              // XXX here is dynamic linking and we should link them manually,
              // because pipelines from parse-launch are not reusable if there
              // is a dynamic linking:
              // https://gstreamer.freedesktop.org/documentation/gstreamer/gstparse.html?gi-language=c#gst_parse_launch
#ifdef USE_HW_PIPELINE
              << "h264parse ! "
              << "nvv4l2decoder name=next ! "
#else
              // << "vp9dec name=next ! "
              << "h264parse ! avdec_h264 name=next ! "
#endif
              << "videoconvert ! "
              // XXX don't use videorate here, process every frame
              //<< "videorate drop-only=TRUE ! "
#ifdef USE_HW_PIPELINE
              << "nvvidconv interpolation-method=5 ! "
              << "video/x-raw,format=" << intermediateHWFormat(output_.capabilities.format) << " ! "
#else
              << "videoscale method=5 ! "
#endif
              << "videoconvert ! "
              << output_.capabilities.type << ','
              << "format=" << output_.capabilities.format << ','
              << "width=" << output_.capabilities.width << ','
              << "height=" << output_.capabilities.height << " ! "
              << "able_ts start-timestamp=" << begin_ts.count() * GST_NSECOND  << " ! "
              << "appsink name=sink max-buffers=5 sync=FALSE";
  // clang-format on

  std::string pipeline_str = pipeline_ss.str();

  GError     *gerr     = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &gerr);
  if (gerr != nullptr) {
    TRY_SET(err, "can not parse input gst pipeline: "s + gerr->message);
    g_error_free(gerr);
    if (pipeline) {
      gst_object_unref(GST_OBJECT(pipeline));
    }
    return -1;
  }

  // add dynamic linking for demuxer
  GstElement *demuxer = gst_bin_get_by_name(GST_BIN(pipeline), "demuxer");
  if (demuxer == nullptr) {
    TRY_SET(err, "can not get 'demuxer' from input pipeline");

    gst_object_unref(GST_OBJECT(pipeline));
    return -1;
  }

  GstElement *next = gst_bin_get_by_name(GST_BIN(pipeline), "next");
  if (next == nullptr) {
    TRY_SET(err, "can not get 'next' element from input pipeline");

    gst_object_unref(GST_OBJECT(pipeline));
    gst_object_unref(GST_OBJECT(demuxer));
    return -1;
  }

  g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), next);

  gst_object_unref(GST_OBJECT(demuxer));
  gst_object_unref(GST_OBJECT(next));

  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (sink == nullptr) {
    TRY_SET(err, "can not get 'sink' element from input pipeline");

    gst_object_unref(GST_OBJECT(pipeline));
    return -1;
  }

  GstBus *bus = gst_element_get_bus(pipeline);
  // gst_element_link_pads(sink, "sink", next, "sink");

  // check that pipeline can be run
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  GstStateChangeReturn ret = gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    // invalid record, ignore
    goto InvalidRecord;
  }

  // get ending timestamp
  if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration_ns) == false) {
    // invalid record, ignore
    goto InvalidRecord;
  }
  end_ts = begin_ts + std::chrono::nanoseconds{duration_ns};

  if (end_ts <= begin_ts) {
    // invalid record, ignore
    goto InvalidRecord;
  }

  // XXX stop the pipeline. It is not a good idea to keep a bunch of gst
  // pipelines running, because they can consume a lot of memory. Instead
  // run them when it is needed
  gst_element_set_state(pipeline, GST_STATE_NULL);

  retval->pipeline      = pipeline;
//   retval->bus           = bus;
  retval->sink          = sink;
  retval->data_filepath = filepath;
  retval->begin_ts      = begin_ts;
  retval->end_ts        = end_ts;

  return 1;

InvalidRecord: // XXX if something go wrong, then we should remove the pipeline
               // properly
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(sink));
  gst_object_unref(GST_OBJECT(bus));
  gst_object_unref(GST_OBJECT(pipeline));

  return 0;
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
