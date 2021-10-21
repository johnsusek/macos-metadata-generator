#pragma once

#include "MetaVisitor.h"
#include "TypeEntities.h"
#include "Utils/Noncopyable.h"
#include "Utils/StringUtils.h"
#include <clang/Basic/Module.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclObjC.h>
#include <iostream>
#include <llvm/ADT/iterator_range.h>
#include <map>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include "yaml-cpp/yaml.h"

using namespace std;

namespace Meta {
class MetaData {
public:
  static map<string, YAML::Node> attributesLookup;
  static map<string, string> apiNotes;
  static bool populateModule(string moduleName);
  static bool populateModuleAttrs(string moduleName);
  static bool getUnavailableInSwift(::Meta::Meta* meta, ::Meta::Meta* owner);
  static string dumpDeclComments(::Meta::Meta* meta, ::Meta::Meta* owner);
  static std::string renamedName(std::string name, std::string ownerKey = "");
  static string lookupApiNotes(string type);
private:
};
}
