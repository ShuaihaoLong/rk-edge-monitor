#include <gst/app/gstappsink.h>
#include <gst/gst.h>

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    gchar* version = gst_version_string();
    g_print("%s\n", version);
    g_free(version);

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=RGB,width=320,height=240 ! "
        "appsink name=sink sync=false max-buffers=1", &error);
    if (error != nullptr || pipeline == nullptr) {
        g_printerr("Pipeline creation failed: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        if (pipeline) gst_object_unref(pipeline);
        gst_deinit();
        return 1;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    const auto state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstSample* sample = state == GST_STATE_CHANGE_FAILURE ? nullptr :
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
    const bool ok = sample != nullptr;
    if (ok) {
        g_print("PASS: received one test frame\n");
        gst_sample_unref(sample);
    } else {
        g_printerr("FAIL: no test frame within timeout; rerun with GST_DEBUG=3\n");
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    gst_deinit();
    return ok ? 0 : 1;
}
