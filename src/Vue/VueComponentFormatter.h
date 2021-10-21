#pragma once

#include "TypeScript/DocSetManager.h"
#include "Meta/MetaEntities.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include <Meta/TypeFactory.h>
#include <sstream>
#include <string>
#include <unordered_set>

namespace TypeScript {
class VueComponentFormatter {
public:
  static VueComponentFormatter current;
  static std::map<std::string, bool> nativeTypes;
  std::vector<std::string> split2(std::string const &input);
  std::string formatTypeId(const ::Meta::IdType& idType, const clang::QualType pointerType, const bool ignorePointerType = false);
  std::string formatTypePointer(const ::Meta::PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType = false);
  std::string formatTypeInterface(const ::Meta::Type& type, const clang::QualType pointerType);
  std::string formatTypeAnonymous(const ::Meta::Type& type, const clang::QualType pointerType);
  std::string formatType(const ::Meta::Type& type, const clang::QualType pointerType, const bool ignorePointerType = false);
  std::string getFunctionProto(const std::vector<::Meta::Type*>& signature, const clang::QualType qualType);
  std::string getDefinitiveSelector(::Meta::MethodMeta* meta);
  std::string getInstanceParamsStr(MethodMeta* meta, BaseClassMeta* owner, bool forConstructor = false);
  std::string getTypeString(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType T, const Type& type, const bool isFuncParam = false);
  std::string vuePropifyTypeName(const std::string& jsName);
  void findAndReplaceIn(std::string& str, std::string searchFor, std::string replaceBy);

private:
};
}
