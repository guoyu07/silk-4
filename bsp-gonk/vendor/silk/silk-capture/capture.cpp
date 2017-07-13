/**
 * Provides a simple video capture class tailored to the needs of silk-capture.js
 *
 * libpreview is used as the backend for bsp-gonk, while the OpenCV
 * VideoCapture API is used for other targets.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "silk-capture"
#ifdef USE_ALOG
#include <utils/Log.h>
#else
#define ALOGE(fmt, args...) printf(LOG_TAG ":E " fmt, ##args)
#define ALOGI(fmt, args...) printf(LOG_TAG ":I " fmt, ##args)
#endif

#ifdef USE_LIBPREVIEW
#include <libpreview.h>
#endif

#include <memory>
#include <nan.h>
#include "Matrix.h"
#include <opencv/highgui.h>

#if CV_MAJOR_VERSION >= 3
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

using std::string;

#define OBJECT_FROM_ARGS(NAME, IND) \
  if (info[IND]->IsObject()) { \
    NAME = info[IND]->ToObject(); \
  } else { \
    Nan::ThrowTypeError("Argument not of expected format"); \
  } \

#define INT_FROM_ARGS(NAME, IND) \
  if (info[IND]->IsInt32()) { \
    NAME = info[IND]->Uint32Value(); \
  }

#define STRING_FROM_ARGS(NAME, IND) \
  if (info[IND]->IsString()) { \
    NAME = string(*Nan::Utf8String(info[IND]->ToString())); \
  }

////
// Holds the current state of a VideoCapture session
class State {
public:
  int deviceId;
  int scaledWidth;
  int scaledHeight;
  bool busy;
#ifdef USE_LIBPREVIEW
  libpreview::Client *client;
  libpreview::FrameFormat frameFormat;
  uv_mutex_t frameDataLock;
  int frameWidth;
  int frameHeight;
  void *frameBuffer;
  libpreview::FrameOwner frameOwner;
#else
  cv::VideoCapture cap;
#endif

  State(int deviceId, int scaledWidth, int scaledHeight)
    : deviceId(deviceId),
      scaledWidth(scaledWidth),
      scaledHeight(scaledHeight),
      busy(true),
#ifdef USE_LIBPREVIEW
      frameWidth(0),
      frameHeight(0),
      frameBuffer(nullptr),
#endif
      opened(false) {
#ifdef USE_LIBPREVIEW
    uv_mutex_init(&frameDataLock);
#endif
  }

  bool open() {
    if (opened) {
      ALOGE("State already open");
      return true;
    }

    if (!busy) {
      ALOGE("State should be busy during open");
      return true;
    }

#ifdef USE_LIBPREVIEW
    client = libpreview::open(OnFrameCallback, OnAbandonedCallback, this);
    opened = client != NULL;
#else
    cap.open(deviceId);
    opened = cap.isOpened();
#endif
    if (opened) {
      busy = false;
    }
    return opened;
  }

  ~State() {
    shutdown();
#ifdef USE_LIBPREVIEW
    uv_mutex_destroy(&frameDataLock);
#endif
  }

  void shutdown() {
#ifdef USE_LIBPREVIEW
    if (client != NULL) {
      client->stopFrameCallback();

      // Null client while holding frameDataLock, to ensure that another thread
      // isn't sitting in OnFrameCallback() when libpreview is closed
      uv_mutex_lock(&frameDataLock);
      auto localclient = client;
      client = NULL;
      uv_mutex_unlock(&frameDataLock);

      // Release the current frameBuffer
      uv_mutex_lock(&frameDataLock);
      if (frameBuffer != NULL) {
        localclient->releaseFrame(frameOwner);
        frameBuffer = NULL;
      }
      uv_mutex_unlock(&frameDataLock);

      // Shutdown calls to OnFrameCallback and/or OnAbandonedCallback
      localclient->release();

      // Grab then release frameDataLock again, to ensure that a thread didn't
      // sneak into OnFrameCallback() before localclient was really deleted.
      uv_mutex_lock(&frameDataLock);
      uv_mutex_unlock(&frameDataLock);
      ALOGI("Shutdown complete");
    }
#else
    cap.release();
#endif
  }

private:
  bool opened;

#ifdef USE_LIBPREVIEW
  static void OnAbandonedCallback(void *userData);
  static void OnFrameCallback(void *userData,
                              void* buffer,
                              libpreview::FrameFormat format,
                              size_t width,
                              size_t height,
                              libpreview::FrameOwner frameOwner);
#endif
};


/*
 * Implements the 'VideoCapture' Javascript class
 */
class VideoCapture: public Nan::ObjectWrap {
friend class VideoCaptureFrameWorker;
friend class VideoCaptureCustomFrameWorker;
public:
  static void Init(v8::Local<v8::Object> exports);

private:
  explicit VideoCapture(State *state): state(state) {}
  ~VideoCapture() {}
  static void New(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void Read(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void ReadCustom(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void Close(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static Nan::Persistent<v8::Function> constructor;

  std::shared_ptr<State> state;
};


class VideoCaptureOpenWorker : public Nan::AsyncWorker {
 public:
  explicit VideoCaptureOpenWorker(std::weak_ptr<State> weakState,
                                  Nan::Callback *callback)
    : Nan::AsyncWorker(callback),
      weakState(weakState) {}
  virtual ~VideoCaptureOpenWorker() {}

  void Execute() {
    auto state = weakState.lock();
    if (state) {
      if (!state->open()) {
        SetErrorMessage("Unable to open camera");
      }
    }
  }

 private:
  std::weak_ptr<State> weakState;
};


/*
 * Async worker used to process the next frame
 */
class VideoCaptureFrameWorker : public Nan::AsyncWorker {
public:
  explicit VideoCaptureFrameWorker(std::weak_ptr<State> weakState,
                                   bool grabAll,
                                   v8::Local<v8::Object> im,
                                   v8::Local<v8::Object> imRGB,
                                   v8::Local<v8::Object> imGray,
                                   v8::Local<v8::Object> imScaledGray,
                                   Nan::Callback *callback)
    : Nan::AsyncWorker(callback),
      weakState(weakState),
      grabAll(grabAll) {
    destIm.Reset(im);
    destRGB.Reset(imRGB);
    destGray.Reset(imGray);
    destScaledGray.Reset(imScaledGray);
  }
  virtual ~VideoCaptureFrameWorker() {}

  void Execute() {
    auto state = weakState.lock();
    if (!state) {
      SetErrorMessage("Camera gone");
      return;
    }

#ifdef USE_LIBPREVIEW
    if (state->frameBuffer == NULL) {
      SetErrorMessage("no frame yet");
      return;
    }
    uv_mutex_lock(&state->frameDataLock);
    switch (state->frameFormat) {
      default:
        // Porting error.  Nothing to do but soldier on...
        ALOGE("Warning: Unknown frame format: %d\n", state->frameFormat);
        //fall through
      case libpreview::FRAMEFORMAT_YVU420SP: {
        // Setup a Matrix object to store the remote image using the
        // height/width that OpenCV expects for a YVU420 semi-planar format
        cv::Mat remote(
            state->frameHeight * 3 / 2,
            state->frameWidth,
            CV_8UC1,
            state->frameBuffer
        );

        im = remote.clone();
        if (grabAll) {
          // There is no CY_YVU420sp2RGB, so use CY_YUV420sp2BGR to achieve the same
          //                 ^^      ^ ^             ^^      ^ ^
          cv::cvtColor(remote, rgb, CV_YUV420sp2BGR, 0);

          cv::Mat remoteGray(
              state->frameHeight,
              state->frameWidth,
              CV_8UC1,
              state->frameBuffer
          );
          gray = remoteGray.clone();
        }
        break;
      }
      case libpreview::FRAMEFORMAT_RGB: {
        cv::Mat remote(
            state->frameHeight,
            state->frameWidth,
            CV_8UC3,
            state->frameBuffer
        );
        im = remote.clone();
        if (grabAll) {
          rgb = remote.clone();
          cv::cvtColor(rgb, gray, CV_RGB2GRAY, 0);
        }
        break;
      }
    }
    uv_mutex_unlock(&state->frameDataLock);
#else
    if (!state->cap.grab()) {
      SetErrorMessage("grab failed");
      return;
    }
    if (!state->cap.retrieve(im)) {
      SetErrorMessage("retrieve failed");
      return;
    }
    if (grabAll) {
      rgb = im.clone();
      cv::cvtColor(im, gray, CV_RGB2GRAY, 0);
    }
#endif

    if (grabAll) {
      cv::resize(
          gray,
          scaledGray,
          cv::Size(state->scaledWidth, state->scaledHeight),
          0, 0,
          cv::INTER_LINEAR
      );
    }
  }

  void HandleErrorCallback() {
    auto state = weakState.lock();
    if (state) {
      state->busy = false;
      Nan::AsyncWorker::HandleErrorCallback();
    }
  }

  void HandleOKCallback() {
    auto state = weakState.lock();
    if (state) {
      state->busy = false;

      Nan::HandleScope scope;
      node_opencv::Matrix *mat;

      v8::Local<v8::Object> localIm = Nan::New(destIm);
      mat = Nan::ObjectWrap::Unwrap<node_opencv::Matrix>(localIm);
      mat->mat = im;
      destIm.Reset();

      if (grabAll) {
        v8::Local<v8::Object> localRGB = Nan::New(destRGB);
        mat = Nan::ObjectWrap::Unwrap<node_opencv::Matrix>(localRGB);
        mat->mat = rgb;
        destRGB.Reset();

        v8::Local<v8::Object> localGray = Nan::New(destGray);
        mat = Nan::ObjectWrap::Unwrap<node_opencv::Matrix>(localGray);
        mat->mat = gray;
        destGray.Reset();

        v8::Local<v8::Object> localScaledGray = Nan::New(destScaledGray);
        mat = Nan::ObjectWrap::Unwrap<node_opencv::Matrix>(localScaledGray);
        mat->mat = scaledGray;
        destScaledGray.Reset();
      }

      Nan::AsyncWorker::HandleOKCallback();
    }
  }

private:
  cv::Mat im;
  cv::Mat rgb;
  cv::Mat gray;
  cv::Mat scaledGray;

  Nan::Persistent<v8::Object> destIm;
  Nan::Persistent<v8::Object> destRGB;
  Nan::Persistent<v8::Object> destGray;
  Nan::Persistent<v8::Object> destScaledGray;
  std::weak_ptr<State> weakState;
  bool grabAll;
};

/*
 * Async worker used to process the next custom frame
 */
class VideoCaptureCustomFrameWorker : public Nan::AsyncWorker {
public:
  explicit VideoCaptureCustomFrameWorker(std::weak_ptr<State> weakState,
                                   v8::Local<v8::Object> im,
                                   string format,
                                   int width,
                                   int height,
                                   Nan::Callback *callback)
    : Nan::AsyncWorker(callback),
      weakState(weakState),
      format(format),
      width(width),
      height(height) {
    destIm.Reset(im);
  }
  virtual ~VideoCaptureCustomFrameWorker() {}

  void Execute() {
    auto state = weakState.lock();
    if (!state) {
      SetErrorMessage("Camera gone");
      return;
    }

    if ((format != "yvu420sp") && (format != "rgb")) {
      SetErrorMessage("unknown custom preview format");
      return;
    }

#ifdef USE_LIBPREVIEW
    if (state->frameBuffer == NULL) {
      SetErrorMessage("no frame yet");
      return;
    }
    uv_mutex_lock(&state->frameDataLock);
    switch (state->frameFormat) {
      default:
        // Porting error.  Nothing to do but soldier on...
        ALOGE("Warning: Unknown frame format: %d\n", state->frameFormat);
        //fall through
      case libpreview::FRAMEFORMAT_YVU420SP: {
        // Setup a Matrix object to store the remote image using the
        // height/width that OpenCV expects for a YVU420 semi-planar format
        cv::Mat remote(
            state->frameHeight * 3 / 2,
            state->frameWidth,
            CV_8UC1,
            state->frameBuffer
        );

        if (format == "yvu420sp") {
          im = remote.clone();
        } else if (format == "rgb") {
          cv::cvtColor(remote, im, CV_YUV420sp2BGR, 0);
        }
        break;
      }
      case libpreview::FRAMEFORMAT_RGB: {
        if (format != "rgb") {
          SetErrorMessage("Only rgb preview format is supported");
          return;
        }

        cv::Mat remote(
            state->frameHeight,
            state->frameWidth,
            CV_8UC3,
            state->frameBuffer
        );
        im = remote.clone();
        break;
      }
    }
    uv_mutex_unlock(&state->frameDataLock);
#else
    if (!state->cap.grab()) {
      SetErrorMessage("grab failed");
      return;
    }
    if (!state->cap.retrieve(im)) {
      SetErrorMessage("retrieve failed");
      return;
    }
#endif
    cv::Size s = im.size();
    if ((width != s.width) ||
        (height != (format == "yvu420sp" ? s.height / 3 * 2 : s.height))) {
        // height in yuv420sp is not correct; adjust when comparing
      if (format == "yvu420sp") {
        SetErrorMessage("Cannot resize in yvu420sp");
        return;
      } else {
        cv::resize(im, im, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
      }
    }
  }

  void HandleErrorCallback() {
    auto state = weakState.lock();
    if (state) {
      state->busy = false;
      Nan::AsyncWorker::HandleErrorCallback();
    }
  }

  void HandleOKCallback() {
    auto state = weakState.lock();
    if (state) {
      state->busy = false;
      Nan::HandleScope scope;
      node_opencv::Matrix *mat;

      v8::Local<v8::Object> localIm = Nan::New(destIm);
      mat = Nan::ObjectWrap::Unwrap<node_opencv::Matrix>(localIm);
      mat->mat = im;
      destIm.Reset();

      Nan::AsyncWorker::HandleOKCallback();
    }
  }

private:
  cv::Mat im;

  Nan::Persistent<v8::Object> destIm;
  std::weak_ptr<State> weakState;
  string format;
  int width;
  int height;
};


class VideoCaptureCloseWorker : public Nan::AsyncWorker {
 public:
  explicit VideoCaptureCloseWorker(std::shared_ptr<State> state,
                                   Nan::Callback *callback)
    : Nan::AsyncWorker(callback),
      state(state) {}
  virtual ~VideoCaptureCloseWorker() {}

  void Execute() {
    state->shutdown();
  }

 private:
  std::shared_ptr<State> state;
};

Nan::Persistent<v8::Function> VideoCapture::constructor;

void VideoCapture::Init(v8::Local<v8::Object> exports) {
  Nan::HandleScope scope;

  // Prepare constructor template
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("VideoCapture").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "read", Read);
  Nan::SetPrototypeMethod(tpl, "readCustom", ReadCustom);
  Nan::SetPrototypeMethod(tpl, "close", Close);

  constructor.Reset(tpl->GetFunction());
  exports->Set(Nan::New("VideoCapture").ToLocalChecked(), tpl->GetFunction());
}

NAN_METHOD(VideoCapture::New) {
  if (!info.IsConstructCall()) {
    // Invoked as plain function `VideoCapture(...)`, turn into construct call.
    const int argc = 4;
    v8::Local<v8::Value> argv[argc] = { info[0], info[1], info[2], info[3] };
    v8::Local<v8::Function> cons = Nan::New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance(argc, argv));
    return;
  }

  // Invoked as constructor: `new VideoCapture(deviceId,  )`

  if (info.Length() < 4 ||
      !info[0]->IsInt32() ||
      !info[1]->IsInt32() ||
      !info[2]->IsInt32() ||
      !info[3]->IsFunction()) {
    Nan::ThrowTypeError("VideoCapture expects four arguments: "
      "deviceId, scaledWidth, scaledHeight, callback");
    return;
  }

  int deviceId = info[0]->ToInt32()->Value();
  int scaledWidth = info[1]->ToInt32()->Value();
  int scaledHeight = info[2]->ToInt32()->Value();
  Nan::Callback *callback = NULL;
  callback = new Nan::Callback(info[3].As<v8::Function>());

  State *state = new State(deviceId, scaledWidth, scaledHeight);
  VideoCapture* self = new VideoCapture(state);
  self->Wrap(info.This());

  Nan::AsyncQueueWorker(new VideoCaptureOpenWorker(self->state, callback));
  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(VideoCapture::Read) {
  VideoCapture* self = ObjectWrap::Unwrap<VideoCapture>(info.Holder());

  if (!self->state || self->state->busy) {
    Nan::ThrowError("Busy");
    return;
  }

  if ((info.Length() != 2) && (info.Length() != 5)) {
    Nan::ThrowError("Insufficient number of arguments provided");
  }

  v8::Local<v8::Object> im;
  v8::Local<v8::Object> imRGB;
  v8::Local<v8::Object> imGray;
  v8::Local<v8::Object> imScaledGray;
  Nan::Callback *callback = NULL;
  bool grabAll = false;

  if (info.Length() == 2) { // Only raw frame is requested
    OBJECT_FROM_ARGS(im, 0);
    callback = new Nan::Callback(info[1].As<v8::Function>());
  } else {
    OBJECT_FROM_ARGS(im, 0);
    OBJECT_FROM_ARGS(imRGB, 1);
    OBJECT_FROM_ARGS(imGray, 2);
    OBJECT_FROM_ARGS(imScaledGray, 3);
    callback = new Nan::Callback(info[4].As<v8::Function>());
    grabAll = true;
  }

  self->state->busy = true;
  Nan::AsyncQueueWorker(
    new VideoCaptureFrameWorker(
      self->state,
      grabAll,
      im,
      imRGB,
      imGray,
      imScaledGray,
      callback
    )
  );
}

NAN_METHOD(VideoCapture::ReadCustom) {
  VideoCapture* self = ObjectWrap::Unwrap<VideoCapture>(info.Holder());

  if (!self->state || self->state->busy) {
    Nan::ThrowError("Busy");
    return;
  }

  if (info.Length() != 5) {
    Nan::ThrowError("Insufficient number of arguments provided");
  }

  v8::Local<v8::Object> im;
  Nan::Callback *callback = NULL;
  string format;
  int width = 0;
  int height = 0;

  OBJECT_FROM_ARGS(im, 0);
  STRING_FROM_ARGS(format, 1);
  INT_FROM_ARGS(width, 2);
  INT_FROM_ARGS(height, 3);
  callback = new Nan::Callback(info[4].As<v8::Function>());

  self->state->busy = true;
  Nan::AsyncQueueWorker(
    new VideoCaptureCustomFrameWorker(
      self->state,
      im,
      format,
      width,
      height,
      callback
    )
  );
}

NAN_METHOD(VideoCapture::Close) {
  VideoCapture* self = ObjectWrap::Unwrap<VideoCapture>(info.Holder());

  if (info.Length() != 1) {
    Nan::ThrowError("Insufficient number of arguments provided");
  }

  Nan::Callback *callback = NULL;
  callback = new Nan::Callback(info[0].As<v8::Function>());

  auto closeWorker = new VideoCaptureCloseWorker(self->state, callback);
  // Release our reference *after* closeWorker holds a reference, to prevent the
  // state from getting destructed on this thread (the main node thread)
  self->state = nullptr;
  Nan::AsyncQueueWorker(closeWorker);
}

#ifdef USE_LIBPREVIEW
void State::OnAbandonedCallback(void *userData) {
  State *state = static_cast<State*>(userData);
  uv_mutex_lock(&state->frameDataLock);
  if (state->frameBuffer != NULL) {
    state->client->releaseFrame(state->frameOwner);
    state->frameBuffer = NULL;
  }
  uv_mutex_unlock(&state->frameDataLock);
}

void State::OnFrameCallback(void *userData,
                            void* buffer,
                            libpreview::FrameFormat format,
                            size_t width,
                            size_t height,
                            libpreview::FrameOwner frameOwner) {
  State *state = static_cast<State *>(userData);

  uv_mutex_lock(&state->frameDataLock);
  if (state->client) {
    if (state->frameBuffer != NULL) {
      state->client->releaseFrame(state->frameOwner);
    }
    state->frameBuffer = buffer;
    state->frameOwner = frameOwner;
    state->frameFormat = format;
    state->frameWidth = width;
    state->frameHeight = height;
  }
  uv_mutex_unlock(&state->frameDataLock);
}
#endif

void Capture_Init(v8::Local<v8::Object> exports) {
  VideoCapture::Init(exports);
}
