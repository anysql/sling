// Copyright 2018 Google Inc.
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

#ifndef SLING_PYAPI_PYMYELIN_H_
#define SLING_PYAPI_PYMYELIN_H_

#include "sling/myelin/compiler.h"
#include "sling/pyapi/pybase.h"

namespace sling {

// Utility class for holding on to internal memory buffers defined in other
// Python objects. This uses the Python buffer interface to get direct access
// to the internal memory representation of other Python objects like
// memoryview and numpy arrays, so these do not need to be copied in the
// Myelin flows.
class PyBuffers {
 public:
  ~PyBuffers();
  Py_buffer *GetBuffer(PyObject *obj);
 private:
  std::vector<Py_buffer *> views_;
};

// Python wrapper for Myelin compiler.
struct PyCompiler : public PyBase {
  // Initialize wrapper.
  int Init(PyObject *args, PyObject *kwds);

  // Deallocate wrapper.
  void Dealloc();

  // Compile flow.
  PyObject *Compile(PyObject *arg);

  // Import Python flow into Myelin flow.
  static bool ImportFlow(PyObject *pyflow, myelin::Flow *flow,
                         PyBuffers *buffers);

  // Import attributes for flow artifact.
  static bool ImportAttributes(PyObject *obj, myelin::Attributes *attrs);

  // Get string attribute for object.
  static const char *PyStrAttr(PyObject *obj, const char *name);

  // Get integer attribute for object.
  static int PyIntAttr(PyObject *obj, const char *name);

  // Get attribute for object. Returns new reference.
  static PyObject *PyAttr(PyObject *obj, const char *name);

  // Myelin compiler.
  myelin::Compiler *compiler;

  // Registration.
  static PyTypeObject type;
  static PyMethodTable methods;
  static void Define(PyObject *module);
};

// Python wrapper for Myelin network.
struct PyNetwork : public PyBase {
  // Initialize wrapper.
  int Init(myelin::Network *net);

  // Deallocate wrapper.
  void Dealloc();

  // Look up global tensor in network.
  PyObject *LookupTensor(PyObject *key);

  // Myelin network.
  myelin::Network *net;

  // Registration.
  static PyTypeObject type;
  static PyMappingMethods mapping;
  static PyMethodTable methods;
  static void Define(PyObject *module);
};

// Python wrapper for Myelin tensor data.
struct PyTensor : public PyBase {
  // Initialize wrapper.
  int Init(PyObject *owner, char *data, const myelin::Tensor *format);

  // Deallocate wrapper.
  void Dealloc();

  // Return tensor name.
  PyObject *Name();

  // Return tensor rank.
  PyObject *Rank();

  // Return tensor shape.
  PyObject *Shape();

  // Return tensor data type.
  PyObject *Type();

  // Return tensor as string.
  PyObject *Str();

  // Get element from tensor.
  PyObject *GetElement(PyObject *index);

  // Assign value to tensor element.
  int SetElement(PyObject *index, PyObject *value);

  // Buffer interface for accessing tensor data.
  int GetBuffer(Py_buffer *view, int flags);
  void ReleaseBuffer(Py_buffer *view);

  // Get shape and stides. There are allocated lazily.
  Py_ssize_t *GetShape();
  Py_ssize_t *GetStrides();

  // Return tensor type as Python type format string.
  char *GetFormat() {
    return const_cast<char *>(myelin::TypeTraits::of(format->type()).pytype());
  }

  // Get address of element in tensor.
  char *GetReference(PyObject *index);

  // Reference for keeping data alive.
  PyObject *owner;

  // Raw data for tensor.
  char *data;

  // Tensor format.
  const myelin::Tensor *format;

  // Shape and strides in Python format.
  Py_ssize_t *shape;
  Py_ssize_t *strides;

  // Registration.
  static PyTypeObject type;
  static PyMappingMethods mapping;
  static PyBufferProcs buffer;
  static PyMethodTable methods;
  static void Define(PyObject *module);
};

}  // namespace sling

#endif  // SLING_PYAPI_PYMYELIN_H_

