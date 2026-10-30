# Integers
- Use fixwidth integers from <cstdint>. Do not use pure `int`. Allow values:
    - `int8_t`
    - `uint8_t`
    - `int16_t`
    - `uint16_t`
    - `int32_t`
    - `uint32_t`
    - `int64_t`
    - `uint64_t`
- Limites allow: `size_t`, only **use** for list indices and sizes. Do not use elsewhere

# Enum
- Enum should prefer inherit unsigned int types
- Enum should starts from `0`, and end with a `<ENUM_PREFIX>_MAX`.
- Only the 1st enum shall be numbering as `0`.
- Enum name shall use PascalCase
- Each enum value shall starts with a SCREAMING_SNAKE_CASE prefix of its PascalCase name, follow by the enum name

```cpp
enum DataType : uint16_t {
    DATA_TYPE_UNKNOWN = 0,
    DATA_TYPE_FOO,
    DATA_TYPE_MAX,    // MAX enum
}
```

# Class
- Class name shall use PascalCase
- Member variables shall start with `m_` prefix
- Class method name use camelCase
  - Each method should have input argument with prefix `i_`, output argument prefix with `o_`

```cpp
class SampleHandler {
 public:
  SampleHandler() : m_data(0) {}
  void onChange(const int64_t i_value, void* const o_processedData);
 private:
  int32_t m_data;
};
```

- Functions shall be `static` methods of a class named after their file, not free functions, so a call site shows which file the code comes from (`Codec::encode` lives in `Codec.h` / `Codec.cpp`)
  - Exception: helpers used by a single `*.cpp` file may stay in an anonymous namespace in that file

# Struct
- A `struct` is plain data only: public fields, no methods. Anything else shall be a class
- Struct name shall use PascalCase
- Struct fields use camelCase, with no `m_` prefix

```cpp
struct FrameLimits {
  size_t maxDataPayload = 16000U;
};
```

# File
- Source and test files shall use PascalCase, named after the main class they contain (`Codec.h`, `Codec.cpp`, `CodecTest.cpp`)
- Directories are lowercase (`include/packetmux/definitions/`)
- Files whose name is set by a tool (`CMakeLists.txt`) and documents under `docs/` are exempt

# Constant and global variables
- Avoid use global variables (except for constant), instead use Singleton class
- In case it is absolutely neccessary for global variables, use `g_` prefix
- For constant, prefer `constexpr` over normal constant
- Constant shall be in SCREAMING_SNAKE_CASE.
- Constant shall be scope-specific
  - If a constant use only in 1 class, define it in the "*.cpp" file of that class, before function definition, but after importing
  - Only add a shared constant to header if it is absolutely intended to use between many modules. In this case, create a separate header file for constant definitions

```cpp
// In foo.cpp, DELAY_TIME is used within class foo only
constexpr int64_t DELAY_TIME = 0;

// In definitions/BufferDef.h, MAX_BUFFER_SIZE is shared between multiple classes
constexpr uint64_t MAX_BUFFER_SIZE = 100ULL;
```

# Constness
- By default, **ALL** variables and arguments shall be **const**, unless the variable is **specifically intended** to mutate

```cpp
// i_data is input, readonly, so shall be const. o_outputData is intended to change the underlying data, but not change where does it point to
void processData(const int64_t i_data, int64_t* const o_outputData) {
    // config isn't changed
    const std::vector<int> config = ReadConfig();

    // assign new value to o_outputData
    *o_outputData = i_data + config.at(0);
};
```

# Namspace
- All file in this repository shall be under namespace `packetmux`
- Avoid use `using` syntax

```cpp
// in frames.h
namespace packetmux {
 class Frame {
  private:
    std::vector<int8_t> m_data;   // use full namespace resolution std::vector
 };
};
```