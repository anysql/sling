// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MYELIN_COMPUTE_H_
#define MYELIN_COMPUTE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "base/types.h"
#include "myelin/flow.h"
#include "string/printf.h"
#include "third_party/jit/code.h"
#include "third_party/jit/cpu.h"

namespace sling {
namespace myelin {

class MacroAssembler;
class Network;
class Cell;
class Step;
class Instance;
class Tensor;
class CUDADevice;

// Element order.
enum Order {ANY_ORDER, ROW_MAJOR, COLUMN_MAJOR, CONFLICTING_ORDER};

// Task state.
enum TaskState {PENDING, ACTIVE, COMPLETED};

// Placement for data and code execution.
enum Placement {NOWHERE = 0x0, HOST = 0x1, DEVICE = 0x2, EVERYWHERE = 0x3};

// Pointer to data in device memory.
typedef uint64 DevicePtr;
#define DEVICE_NULL 0

// Minimum data alignment.
static const int kMinDataAlignment = sizeof(void *);

// Abstract interface for kernel implementing a code generator for an operation.
class Kernel {
 public:
  virtual ~Kernel() = default;

  // Return descriptive name for kernel.
  virtual string Name() = 0;

  // Return location of kernel computation.
  virtual Placement Location() { return HOST; }

  // Return name of operation support by kernel.
  virtual string Operation() = 0;

  // Check if kernel supports generating code for step.
  virtual bool Supports(Step *step) = 0;

  // Let kernel adjust alignment constraints for step.
  virtual void Adjust(Step *step) {}

  // Generate code for step.
  virtual void Generate(Step *step, MacroAssembler *masm) = 0;

  // Number of numeric operations kernel performs for step.
  virtual int Complexity(const Step *step) { return -1; }
};

// Library of kernels for implemeting operations.
class Library : public Transformations {
 public:
  typedef std::vector<Kernel *> Kernels;

  ~Library();

  // Registers kernel. Ownership is transferred to the library.
  void Register(Kernel *kernel);

  // Find kernels implementing operation.
  const Kernels &Lookup(const string &op) const;

  // Find kernel and add to singleton library. The singleton library does not
  // own the kernel.
  bool Singleton(const string &op,
                 const string &name,
                 Library *singleton) const;

 private:
  // Map from op name to kernels implemeting the op.
  std::unordered_map<string, Kernels> kernels_;

  // Whether kernels are owned by library.
  bool owns_kernels_ = true;

  // Empty kernel list.
  Kernels no_kernels_;
};

// A task is an asynchronous function that can be run in parallel with the main
// computation. The task structures are stored in the instance blocks.
struct Task {
  // Function with argument to be executed by task.
  void (*func)(void *arg);
  void *arg;

  // Data field that can be used by runtime for state information.
  void *state;

  // Task id for flow.
  int32 id;

  // Task index for cell.
  int32 index;
};

// Runtime support for network.
class Runtime {
 public:
  typedef void (*TaskFunc)(Task *);
  typedef void (*InstanceFunc)(void *);

  virtual ~Runtime() = default;

  // Return runtime description.
  virtual string Description() { return ""; }

  // Allocate and initialize instance data.
  virtual void AllocateInstance(Instance *instance) = 0;

  // Deallocate instance data.
  virtual void FreeInstance(Instance *instance) = 0;

  // Clear instance data.
  virtual void ClearInstance(Instance *instance) = 0;

  // Check if runtime supports asynchronous execution of steps.
  virtual bool SupportsAsync() = 0;

  // Return runtime function for starting task.
  virtual TaskFunc StartTaskFunc() = 0;

  // Return runtime function for waiting for task completion.
  virtual TaskFunc WaitTaskFunc() = 0;

  // Return runtime function for synchronizing the main task execution. This
  // can return null if no synchronization is needed.
  virtual InstanceFunc SyncMainFunc() { return nullptr; }

  // Return the size of extra instance data needed by runtime. This extra data
  // will be allocated at the beginning of the instance block at offset 0.
  virtual int ExtraInstanceData(Cell *cell) { return 0; }

  // Copy constant tensor to device.
  virtual DevicePtr CopyTensorToDevice(Tensor *tensor) { return DEVICE_NULL; }

  // Remove constant tensor from device.
  virtual void RemoveTensorFromDevice(Tensor *tensor) {}

  // Generate code for copying tensor from host to device.
  virtual void EmitCopyTensorToDevice(Tensor *tensor,
                                      Cell *cell,
                                      int taskidx,
                                      MacroAssembler *masm) {}

  // Generate code for copying tensor from device to host.
  virtual void EmitCopyTensorFromDevice(Tensor *tensor,
                                        Cell *cell,
                                        int taskidx,
                                        MacroAssembler *masm) {}

  // Return CUDA device used by runtime.
  virtual CUDADevice *Device() { return nullptr; }

  // Return runtime function for starting profiler.
  virtual InstanceFunc StartProfilerFunc() { return nullptr; }

  // Return runtime function for stopping profiler.
  virtual InstanceFunc StopProfilerFunc() { return nullptr; }
};

// A tensor is a multi-dimensional array that can be used for constants and
// parameters.
class Tensor {
 public:
  // Set alignment constraints for tensor.
  void Align(const Shape &align);

  // Set alignment constraint for last dimension of tensor.
  void AlignLast(int align);

  // Ensure same alignment as other tensor.
  void SameAlign(Tensor *other);

  // Ensure compatible alignment modulo broadcasting with other tensor.
  void CompatibleAlign(Tensor *other);

  // Check if tensor can support order.
  bool SupportsOrder(Order order);

  // Set required element order.
  void SetRequiredOrder(Order order);

  // Set minimum byte alignment for tensor.
  void SetMiniumAlignment(int alignment);

  // Check if tensor has the same shape as another tensor.
  bool HasSameShape(const Tensor *other) const;

  // Check if tensor shape that is broadcast compatible with another tensor.
  bool Compatible(const Tensor *other) const;

  // Check if tensor is a scalar.
  bool IsScalar() const { return rank() == 0; }

  // Check if tensor is a vector.
  bool IsVector() const { return rank() == 1; }

  // Check if tensor is a matrix.
  bool IsMatrix() const { return rank() == 2; }

  // Tensor name for parameter or constant.
  const string &name() const { return name_; }

  // Data type for tensor elements.
  Type type() const { return type_; }

  // Reference to tensor.
  bool ref() const { return ref_; }
  void set_ref(bool ref) { ref_ = ref; }

  // Tensor shape.
  const Shape &shape() const { return shape_; }
  int rank() const { return shape_.rank(); }
  int dim(int d) const { return shape_.dim(d); }

  // Alignment requirement for each dimension.
  const Shape &alignment() const { return alignment_; }
  int alignment(int d) const { return alignment_.dim(d); }

  // Tensor shape after alignment.
  const Shape &aligned() const { return aligned_; }
  int aligned(int d) const { return aligned_.dim(d); }

  // Size (in bytes) of each dimension after alignment.
  const Shape &stride() const { return stride_; }
  int stride(int d) const { return stride_.dim(d); }

  // Padding (in bytes) to each dimension.
  int padding(int d) const { return (aligned(d) - dim(d)) * stride(d); }

  // Total size (in bytes) for tensor instance.
  int size() const { return size_; }

  // Number of elements in tensor.
  int elements() const { return shape_.elements(); }

  // Value for constant tensor. Returns null for parameters.
  char *data() const { return data_; }

  // Pointer to constant tensor on device.
  DevicePtr device_data() const { return device_data_; }

  // Size (in bytes) of elements in tensor.
  int element_size() const { return TypeTraits::of(type_).size(); }

  // Offset in data instance block. Returns -1 for constants and tensors that
  // are not stored on the host.
  int offset() const { return offset_; }

  // Offset in device data instance block. Returns -1 for constants and tensors
  // that are not stored on the device.
  int device_offset() const { return device_offset_; }

  // Number of bytes allocated for tensor in instance. This takes references
  // into account so these only take up space for one pointer.
  int space() const { return space_; }

  // Byte offset of element in tensor.
  int offset(int r) const { return r * stride(0); }
  int offset(int r, int c) const { return r * stride(0) + c * stride(1); }

  // Index of element in tensor.
  int index(int r) const { return offset(r) / element_size(); }
  int index(int r, int c) const { return offset(r, c) / element_size(); }

  // Check if tensor is a constant.
  bool IsConstant() const {
    return data_ != nullptr || device_data_ != DEVICE_NULL;
  }

  // Return tensor placement.
  Placement placement() const { return placement_; }

  // Add location for placement.
  void AddPlace(Placement place) {
    placement_ = static_cast<Placement>(placement_ | place);
  }

  // Add new location for curent placement.
  void AddNewPlace(Placement place) {
    current_placement_ = static_cast<Placement>(current_placement_ | place);
  }

  // Return the task index for consumers of this tensor or -1 if tensor is
  // consumed by operations in multiple tasks.
  int ConsumerTask() const;

  // Return scalar value.
  template<typename T> T value() const { return *reinterpret_cast<T *>(data_); }

  // Element order.
  Order order() const { return order_; }
  Order required_order() const { return required_order_; }

  // Other tensor that this tensor shares storage with.
  Tensor *shared() const { return shared_; }
  void set_shared(Tensor *shared) { shared_ = shared; }

  // Check if tensor shares the underlying storage with another tensor.
  bool SharedWith(Tensor *other) const {
    return shared_ == other || other->shared_ == this;
  }

  // Other tensor that this tensor shares alignment requirements with.
  Tensor *link() const { return link_; }
  void set_link(Tensor *link) { link_ = link; }

  // Step that produces tensor.
  Step *producer() const { return producer_; }

  // List of steps that uses tensor.
  const std::vector<Step *> consumers() const { return consumers_; }

  // Cell that tensor belongs to.
  Cell *cell() const { return cell_; }

  // Return tensor type as string.
  string TypeString() const;

 private:
  // Offset in data instance block.
  int offset_ = -1;

  // Offset in device data instance block.
  int device_offset_ = -1;

  // Tensor name for parameter or constant.
  string name_;

  // Element data type.
  Type type_ = DT_INVALID;

  // Tensor reference.
  bool ref_ = false;

  // Tensor shape.
  Shape shape_;

  // Alignment requirement for each dimension.
  Shape alignment_;

  // Tensor shape after alignment.
  Shape aligned_;

  // Size of each dimension after alignment.
  Shape stride_;

  // Total size (in bytes) for tensor instance.
  int size_ = 0;

  // Number of bytes allocated for tensor in instance.
  int space_ = 0;

  // Minimum alignment (in bytes) for tensor instance.
  int byte_alignment_ = 1;

  // Element order for data.
  Order order_ = ROW_MAJOR;
  Order required_order_ = ANY_ORDER;

  // Optional other tensor that this tensor shares storage with.
  Tensor *shared_ = nullptr;

  // Optional other tensor that this tensor shares alignment requirements with.
  Tensor *link_ = nullptr;

  // Value for constant tensor (not owned).
  char *data_ = nullptr;

  // Pointer to constant tensor data on device. This is only set for constant
  // tensors that need to be access from the device.
  DevicePtr device_data_ = DEVICE_NULL;

  // Cell that tensor is part of. Constant tensors can be shared.
  Cell *cell_ = nullptr;

  // Step that produces tensor.
  Step *producer_ = nullptr;

  // Steps that consume tensor.
  std::vector<Step *> consumers_;

  // Placement of tensor.
  Placement placement_ = NOWHERE;

  // Current placement of tensor in compilation.
  Placement current_placement_ = NOWHERE;

  // Deferred placement for outputs from asynchronous steps.
  Placement deferred_placement_ = NOWHERE;

  friend class Network;
};

// A step represents an operation that is part of a cell.
class Step {
 public:
  // Step name from flow operation.
  const string &name() const { return name_; }

  // Operation type for step.
  const string &type() const { return type_; }

  // Inputs to step.
  const std::vector<Tensor *> &inputs() const { return inputs_; }
  Tensor *input(int index) const { return inputs_[index]; }
  int indegree() const { return inputs_.size(); }

  // Outputs from step.
  const std::vector<Tensor *> &outputs() const { return outputs_; }
  Tensor *output(int index) const { return outputs_[index]; }
  int outdegree() const { return outputs_.size(); }

  // Kernel used for generating code for step.
  Kernel *kernel() const { return kernel_; }

  // Return the complexity of the cell, i.e. number of numeric operations.
  int complexity() const { return kernel_->Complexity(this); }

  // Cell that this step belongs to.
  Cell *cell() const { return cell_; }

  // Task index in cell for computing the step.
  int task_index() const { return task_index_; }

  // Device placement for kernel computation.
  Placement placement() const { return kernel_->Location(); }

  // Declare the number of general-purpose registers needed by step.
  void SetRegisterUsage(int regs);

  // Declare the number of preserved registers needed by step.
  void SetPreservedRegisterUsage(int regs);

  // Allow in-place operation between input and output. Return true if in-place
  // operation is supported, i.e. the operation must be the only consumer of
  // the input.
  bool AllowInPlace(int input, int output);

  // A step in the main task that runs on the host but depends on inputs
  // produced on the device needs to be synchronized to ensure that the inputs
  // are ready before executing the task. This method checks if a step needs
  // to be synchronized before execution.
  bool NeedsSynchronization();

 private:
   // Step name from flow operation.
   string name_;

  // Operation type for step.
  string type_;

  // Cell that this step belongs to.
  Cell *cell_ = nullptr;

  // Task index in cell for computing the step.
  int task_index_ = -1;

  // Inputs to step.
  std::vector<Tensor *> inputs_;

  // Outputs from step.
  std::vector<Tensor *> outputs_;

  // Kernel used for generating code for step (owned by library).
  Kernel *kernel_ = nullptr;

  friend class Network;
};

// A connector links different (parts of) cells in a network to create recurrent
// connections.
class Connector {
 public:
  ~Connector() { delete type_; }

  // Connector name.
  const string &name() const { return type_->name(); }

  // Connector type.
  Tensor *type() const { return type_; }

  // Size of one element.
  int size() const { return type_->size(); }

  // Connector array alignment (in bytes).
  int alignment() const { return alignment_; }

  // Tensors linked to the connector.
  const std::vector<Tensor *> &links() const { return links_; }

 private:
  // Tensor for connector type.
  Tensor *type_ = nullptr;

  // Tensors linked to the connector.
  std::vector<Tensor *> links_;

  // Connector array alignment (in bytes).
  int alignment_ = kMinDataAlignment;

  friend class Network;
};

// A channel is an array of tensors used for connecting cells in a network.
class Channel {
 public:
  // Initialize empty channel.
  Channel(const Connector *connector) : connector_(connector) {}

  // Delete channel.
  ~Channel();

  // Remove all elements from channel.
  void clear() { resize(0); }

  // Change size of channel.
  void resize(int n);

  // Reserve space for channel elements.
  void reserve(int n);

  // Return pointer to channel element.
  char *at(int index) const {
    return data_ + (index * connector_->size());
  }

  // Add element to channel and return the last element.
  char *push() { resize(size_ + 1); return at(size_ - 1); }

  // Remove the last element from the channel.
  void pop() { resize(size_ - 1); }

  // Return the number of elements in the channel.
  int size() const { return size_; }

 private:
  // Data for the channel.
  char *data_ = nullptr;

  // Number of elements in channel.
  int size_ = 0;

  // Number of allocated elements.
  int capacity_ = 0;

  // Connector describing the element type of the channel.
  const Connector *connector_;
};

// An instance holds all the input, output, and intermediate parameters of a
// cell.
class Instance {
 public:
  // Create data instance.
  Instance(const Cell *cell);

  // Delete data instance.
  ~Instance();

  // Clear instance.
  void Clear();

  // Run cell computation on instance.
  inline void Compute();

  // Get pointer to location of parameter in instance memory.
  template<typename T> T *Get(Tensor *param) {
    DCHECK(param != nullptr);
    DCHECK(!param->IsConstant());
    DCHECK_EQ(Traits<T>().type(), param->type());
    return reinterpret_cast<T *>(data_ + param->offset());
  }

  // Get pointer to location of element of parameter in instance memory.
  template<typename T> T *Get(Tensor *param, int r) {
    DCHECK(param != nullptr);
    DCHECK(!param->IsConstant());
    DCHECK_EQ(Traits<T>().type(), param->type());
    return reinterpret_cast<T *>(data_ + param->offset() + param->offset(r));
  }
  template<typename T> T *Get(Tensor *param, int r, int c) {
    DCHECK(param != nullptr);
    DCHECK(!param->IsConstant());
    DCHECK_EQ(Traits<T>().type(), param->type());
    return reinterpret_cast<T *>(
        data_ + param->offset() + param->offset(r, c));
  }

  // Set link to element in connector channel.
  void Set(Tensor *param, Channel *channel, int index = 0) {
    *reinterpret_cast<char **>(data_ + param->offset()) = channel->at(index);
  }

  // Return parameter as string.
  string ToString(Tensor *param) const;

  // Return all parameters as string.
  string ToString() const;

  // Return pointer to data block for instance.
  char *data() const { return data_; }
  void set_data(char *data) { data_ = data; }

  // Return cell for instance.
  const Cell *cell() const { return cell_; }

  // Return runtime for cell.
  inline Runtime *runtime() const;

  // Number of auxiliary tasks used.
  inline int num_tasks() const;

  // Return task structure for task.
  inline Task *task(int index) const;

  // Return instance size.
  inline int size() const;

  // Return instance alignment.
  inline int alignment() const;

 private:
  // Aligned memory block with parameters.
  char *data_;

  // Cell for instance.
  const Cell *cell_;
};

// A cell contains generated code for executing computation of a function.
class Cell {
 public:
  // Cell name from flow function.
  const string &name() const { return name_; }

  // Cell computation steps.
  const std::vector<Step *> &steps() const { return steps_; }

  // Get parameter.
  Tensor *GetParameter(const string &name) const;

  // Write code to file.
  void WriteCodeToFile(const string &filename) const;

  // Code object for compiled cell.
  const jit::Code &code() const { return code_; }

  // Network that cell is part of.
  Network *network() const { return network_; }

  // Runtime for cell.
  inline Runtime *runtime() const;

  // Size of data instance for cell.
  int instance_size() const { return instance_size_; }

  // Size of device data instance for cell.
  int device_instance_size() const { return device_instance_size_; }

  // Instance alignment.
  int instance_alignment() const { return instance_alignment_; }
  int device_instance_alignment() const { return device_instance_alignment_; }

  // Number of auxiliary tasks used by cell.
  int num_tasks() const { return tasks_.size(); }

  // Convert task index to task id.
  int task(int index) const { return tasks_[index].task; }

  // Get offset of task structure in instance data block.
  int task_offset(int index) const { return tasks_[index].offset; }

  // Tensor with profiling information.
  Tensor *profile() const { return profile_; }

  // Return cell in text format.
  string ToString() const;

 private:
  // Task state information.
  struct TaskInfo {
    TaskInfo(int task) : task(task) {}

    int task;                       // task id in flow
    TaskState state = PENDING;      // task state at current compilation point
    jit::Label entry;               // entry point for task function
    int offset = 0;                 // instance offset for task structure
    Placement placement = NOWHERE;  // placement of task computation
  };

  // Network that cell is part of.
  Network *network_;

  // Cell name.
  string name_;

  // Steps for cell in order of execution (owned by network).
  std::vector<Step *> steps_;

  // Tasks for parallel execution of steps in cell computation.
  std::vector<TaskInfo> tasks_;

  // Number of general-purpose register needed by cell.
  int register_usage_ = 0;

  // Code for running the cell computation.
  jit::Code code_;

  // Size of data instance for cell.
  int instance_size_ = 0;

  // Size of device data instance for cell.
  int device_instance_size_ = 0;

  // Instance alignment.
  int instance_alignment_ = kMinDataAlignment;
  int device_instance_alignment_ = kMinDataAlignment;

  // Tensor with profiling information.
  Tensor *profile_ = nullptr;

  friend class Network;
  friend class Step;
};

// A network is a collection of cells and variables that are compiled as a unit.
class Network {
 public:
  Network();
  ~Network();

  // Compile network to generate code for all the cells.
  bool Compile(const Flow &flow, const Library &library);

  // Load flow from file and compile all the cells.
  bool Compile(const string &flowfile, const Library &library);

  // Get compiled cell.
  Cell *GetCell(const string &name) const;

  // Get connector.
  Connector *GetConnector(const string &name) const;

  // Get parameter.
  Tensor *GetParameter(const string &name) const;

  // Runtime support functions.
  Runtime *runtime() const { return runtime_; }
  void set_runtime(Runtime *runtime) { runtime_ = runtime; }

  // Set element order for parameters.
  void set_parameter_element_order(Order order) {
    parameter_element_order_ = order;
  }

  // Enable debugging by inserting a break point in the generated code.
  void set_debug(bool debug) { debug_ = debug; }

  // Enable profiling by instrumenting code with timestamp timing code.
  void set_profiling(bool profiling) { profiling_ = profiling; }

  // Network cells.
  const std::vector<Cell *> cells() const { return cells_; }

  // Network constants.
  const std::vector<Tensor *> constants() const { return constants_; }

  // Network parameters.
  const std::vector<Tensor *> parameters() const { return parameters_; }

 private:
  // Network cells.
  std::vector<Cell *> cells_;

  // Constants in network, e.g. weight matrices and vectors.
  std::vector<Tensor *> constants_;

  // Parameters in instance blocks (input, output, and intermediate values).
  std::vector<Tensor *> parameters_;

  // Steps for network computation in order of execution.
  std::vector<Step *> steps_;

  // Connections between tensors.
  std::vector<Connector *> connectors_;

  // Parameter names.
  std::unordered_map<string, Tensor *> names_;

  // Memory blocks owned by network.
  std::vector<char *> memory_;

  // Runtime support.
  Runtime *runtime_;

  // Element order for parameters.
  Order parameter_element_order_ = ROW_MAJOR;

  // Debug mode.
  bool debug_ = false;

  // Profiling mode.
  bool profiling_ = false;

  friend class Instance;
};

inline Runtime *Cell::runtime() const {
  return network_->runtime();
}

inline Runtime *Instance::runtime() const {
  return cell_->runtime();
}

inline void Instance::Compute() {
  cell_->code().Execute(data_);
}

inline int Instance::num_tasks() const {
  return cell_->num_tasks();
}

inline Task *Instance::task(int index) const {
  return reinterpret_cast<Task *>(data_ + cell_->task_offset(index));
}

inline int Instance::size() const {
  return cell_->instance_size();
}

inline int Instance::alignment() const {
  return cell_->instance_alignment();
}

}  // namespace myelin
}  // namespace sling

#endif  // MYELIN_COMPUTE_H_

