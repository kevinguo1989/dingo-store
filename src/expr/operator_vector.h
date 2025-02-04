// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
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

#ifndef DINGODB_EXPR_OPERATORVECTOR_H_
#define DINGODB_EXPR_OPERATORVECTOR_H_

#include <vector>

#include "operator.h"
#include "types.h"

namespace dingodb::expr {

class OperatorVector {
 public:
  OperatorVector() : m_vector() {}
  virtual ~OperatorVector() {}

  void Decode(const byte code[], size_t len);

  auto begin() { return m_vector.begin(); }

  auto end() { return m_vector.end(); }

 private:
  std::vector<Operator> m_vector;

  void Add(const Operator &op) { m_vector.push_back(op); }

  template <template <typename> class OP>
  void AddOperatorByType(byte b);

  void AddCastOperator(byte b);
};

}  // namespace dingodb::expr

#endif  // DINGODB_EXPR_OPERATORVECTOR_H_
