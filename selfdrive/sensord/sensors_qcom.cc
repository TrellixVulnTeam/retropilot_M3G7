#include <cutils/log.h>
#include <hardware/sensors.h>
#include <sys/cdefs.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Timers.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include <cutils/properties.h>

#include <dlfcn.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cereal/messaging/messaging.h"
#include "selfdrive/common/swaglog.h"
#include "selfdrive/common/timing.h"
#include "selfdrive/common/util.h"

// ACCELEROMETER_UNCALIBRATED is only in Android O
// https://developer.android.com/reference/android/hardware/Sensor.html#STRING_TYPE_ACCELEROMETER_UNCALIBRATED

#define SENSOR_ACCELEROMETER 15
#define SENSOR_MAGNETOMETER 13
#define SENSOR_GYRO 9
#define SENSOR_MAGNETOMETER_UNCALIBRATED 3
#define SENSOR_GYRO_UNCALIBRATED 8
#define SENSOR_PROXIMITY 0
#define SENSOR_LIGHT 14

ExitHandler do_exit;
volatile sig_atomic_t re_init_sensors = 0;

namespace {

static int load(const char *id,
        const char *path,
        const struct hw_module_t **pHmi)
{
    int status = -EINVAL;
    void *handle = NULL;
    struct hw_module_t *hmi = NULL;

    const char *sym = HAL_MODULE_INFO_SYM_AS_STR;

    handle = dlopen(path, RTLD_NOW);
    if (handle == NULL) {
        char const *err_str = dlerror();
        printf("load: module=%s\n%s\n", path, err_str?err_str:"unknown");
        status = -EINVAL;
        goto done;
    }

    /* Get the address of the struct hal_module_info. */
    hmi = (struct hw_module_t *)dlsym(handle, sym);
    if (hmi == NULL) {
        printf("load: couldn't find symbol %s\n", sym);
        status = -EINVAL;
        goto done;
    }

    /* Check that the id matches */
    if (strcmp(id, hmi->id) != 0) {
        printf("load: id=%s != hmi->id=%s\n", id, hmi->id);
        status = -EINVAL;
        goto done;
    }

    hmi->dso = handle;

    /* success */
    status = 0;

  done:
    if (status != 0) {
        hmi = NULL;
        if (handle != NULL) {
            dlclose(handle);
            handle = NULL;
        }
    } else {
        printf("loaded HAL id=%s path=%s hmi=%p handle=%p\n",
                id, path, hmi, handle);
    }

    *pHmi = hmi;

    return status;
}

int hw_get_module_by_class_x(const char *class_id, const char *inst,
                           const struct hw_module_t **module)
{
  const char *path = "/vendor/lib64/sensors.ssc.so";
  return load(class_id, path, module);
}

//void sigpipe_handler(int sig) {
//  printf("SIGPIPE received");
//  re_init_sensors = true;
//}
//
void sensor_loop() {
  printf("*** sensor loop\n");

  uint64_t frame = 0;
  bool low_power_mode = false;

  while (!do_exit) {
    SubMaster sm({"deviceState"});
    PubMaster pm({"sensorEvents"});

    printf("begin to hw_get_module\n");
    struct sensors_poll_device_t* device;
    struct sensors_module_t* module;

    //int ret = hw_get_module(SENSORS_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
    //int ret = hw_get_module("blueline", (hw_module_t const**)&module);
    //int ret = hw_get_module("ssc", (hw_module_t const**)&module);
    int ret = hw_get_module_by_class_x(SENSORS_HARDWARE_MODULE_ID, NULL, (hw_module_t const**)&module);
    printf("hw_get_module Return Code: %d\n",ret);

    printf("begin to sensors_open\n");
    sensors_open(&module->common, &device);

    // required
    struct sensor_t const* list;
    int count = module->get_sensors_list(module, &list);
    printf("%d sensors found\n", count);

    if (getenv("SENSOR_TEST")) {
      exit(count);
    }

    for (int i = 0; i < count; i++) {
      printf("sensor %4d: %4d %60s  %d-%ld us\n", i, list[i].handle, list[i].name, list[i].minDelay, list[i].maxDelay);
    }

    std::set<int> sensor_types = {
      SENSOR_TYPE_ACCELEROMETER,
      SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED,
      SENSOR_TYPE_MAGNETIC_FIELD,
      SENSOR_TYPE_GYROSCOPE_UNCALIBRATED,
      SENSOR_TYPE_GYROSCOPE,
      SENSOR_TYPE_PROXIMITY,
      SENSOR_TYPE_LIGHT,
    };

    std::map<int, int64_t> sensors = {
      {SENSOR_GYRO_UNCALIBRATED, ms2ns(10)},
      {SENSOR_MAGNETOMETER_UNCALIBRATED, ms2ns(100)},
      {SENSOR_ACCELEROMETER, ms2ns(10)},
      {SENSOR_GYRO, ms2ns(10)},
      {SENSOR_MAGNETOMETER, ms2ns(100)},
      {SENSOR_PROXIMITY, ms2ns(100)},
      {SENSOR_LIGHT, ms2ns(100)}
    };

    // sensors needed while offroad
    std::set<int> offroad_sensors = {
      SENSOR_LIGHT,
      SENSOR_ACCELEROMETER,
      SENSOR_GYRO_UNCALIBRATED,
    };

    // init all the sensors
    for (auto &s : sensors) {
      device->activate(device, s.first, 0);
      device->activate(device, s.first, 1);
      device->setDelay(device, s.first, s.second);
    }

    // TODO: why is this 16?
    static const size_t numEvents = 16;
    sensors_event_t buffer[numEvents];

    while (!do_exit) {
      int n = device->poll(device, buffer, numEvents);
      if (n == 0) continue;
      if (n < 0) {
        printf("sensor_loop poll failed: %d", n);
        continue;
      }

      int log_events = 0;
      for (int i=0; i < n; i++) {
        //if (buffer[i].type == 35) {
        //  continue;
        //}
        //printf("buffer type: %d\n", buffer[i].type);
        if (sensor_types.find(buffer[i].type) != sensor_types.end()) {
          log_events++;
        }
      }

      MessageBuilder msg;
      auto sensor_events = msg.initEvent().initSensorEvents(log_events);

      int log_i = 0;
      for (int i = 0; i < n; i++) {

        const sensors_event_t& data = buffer[i];

        if (sensor_types.find(data.type) == sensor_types.end()) {
          continue;
        }

        auto log_event = sensor_events[log_i];
        log_event.setSource(cereal::SensorEventData::SensorSource::ANDROID);
        log_event.setVersion(data.version);
        log_event.setSensor(data.sensor);
        log_event.setType(data.type);
        log_event.setTimestamp(data.timestamp);

        switch (data.type) {
        case SENSOR_TYPE_ACCELEROMETER: {
          auto svec = log_event.initAcceleration();
          svec.setV(data.acceleration.v);
          svec.setStatus(data.acceleration.status);
          break;
        }
        case SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED: {
          auto svec = log_event.initMagneticUncalibrated();
          // assuming the uncalib and bias floats are contiguous in memory
          kj::ArrayPtr<const float> vs(&data.uncalibrated_magnetic.uncalib[0], 6);
          svec.setV(vs);
          break;
        }
        case SENSOR_TYPE_MAGNETIC_FIELD: {
          auto svec = log_event.initMagnetic();
          svec.setV(data.magnetic.v);
          svec.setStatus(data.magnetic.status);
          break;
        }
        case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED: {
          auto svec = log_event.initGyroUncalibrated();
          // assuming the uncalib and bias floats are contiguous in memory
          kj::ArrayPtr<const float> vs(&data.uncalibrated_gyro.uncalib[0], 6);
          svec.setV(vs);
          break;
        }
        case SENSOR_TYPE_GYROSCOPE: {
          auto svec = log_event.initGyro();
          svec.setV(data.gyro.v);
          svec.setStatus(data.gyro.status);
          break;
        }
        case SENSOR_TYPE_PROXIMITY: {
          log_event.setProximity(data.distance);
          break;
        }
        case SENSOR_TYPE_LIGHT:
          log_event.setLight(data.light);
          break;
        }

        log_i++;
      }

      pm.send("sensorEvents", msg);
      //printf("send one sensorEvents msg..\n");

      if (re_init_sensors) {
        printf("Resetting sensors");
        re_init_sensors = false;
        break;
      }

      // Check whether to go into low power mode at 5Hz
      if (frame % 20 == 0) {
        sm.update(0);
        bool offroad = !sm["deviceState"].getDeviceState().getStarted();
        if (low_power_mode != offroad) {
          for (auto &s : sensors) {
            device->activate(device, s.first, 0);
            if (!offroad || offroad_sensors.find(s.first) != offroad_sensors.end()) {
              device->activate(device, s.first, 1);
            }
          }
          low_power_mode = offroad;
        }
      }

      frame++;
    }
    sensors_close(device);
  }
}

}// Namespace end

int main(int argc, char *argv[]) {
  //setpriority(PRIO_PROCESS, 0, -18);
  //signal(SIGPIPE, (sighandler_t)sigpipe_handler);

  printf("begin to sensor_loop...\n");
  sensor_loop();

  return 0;
}
