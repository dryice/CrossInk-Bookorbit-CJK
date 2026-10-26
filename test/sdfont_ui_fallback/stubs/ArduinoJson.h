#pragma once

// Host stub of ArduinoJson.h: the settings header only names JsonDocument in
// declarations. PersistableStore's inline bodies (instantiated with the
// settings class) default-construct a JsonDocument and convert it to a
// JsonVariantConst; the file I/O symbols are defined in the test itself.
class JsonVariantConst {
 public:
  JsonVariantConst() = default;
};

class JsonDocument {
 public:
  JsonDocument() = default;
  template <typename T>
  T as() const {
    return T{};
  }
};
