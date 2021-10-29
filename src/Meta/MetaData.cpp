#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include "Utils/StringUtils.h"
#include "Meta/Utils.h"
#include "MetaData.h"
#include "MetaFactory.h"
#include "yaml-cpp/yaml.h"

using namespace std;
namespace Meta {

// MetaData loads and provides metadata from .apinotes, swift attributes json files,
// and parsed decl attributes to offer a unified source of information
// about a declaration.
//
// This includes things like whether the decl has been renamed, deprecated,
// or is otherwise unavailable.
//
// Using only the parsed objc attrs provides an incomplete picture so we need
// to use outside metadata
string sdkVersion = getenv("SDKVERSION");
string sdkRoot = "/Library/Developer/CommandLineTools/SDKs/MacOSX" + sdkVersion + ".sdk";
string dataRoot = getenv("DATAPATH");
string attrLookupRoot = dataRoot + "/attributes";

map<string, YAML::Node> MetaData::attributesLookup = {};
map<string, string> MetaData::apiNotes = {};


//vector<string> getRenamedForSwift2(::Meta::Meta* meta, ::Meta::Meta* owner) {
//  string key = owner->jsName + "." + meta->name;
//  string key2 = owner->swiftName + "." + meta->name;
//  YAML::Node renamedFromAttrs;
//  vector<string> tokens;
//
//  if (attributesLookup[key]["renamed"]) {
//    renamedFromAttrs = attributesLookup[key]["renamed"];
//  }
//  else if (attributesLookup[key2]["renamed"]) {
//    renamedFromAttrs = attributesLookup[key2]["renamed"];
//  }
//
//  if (renamedFromAttrs != NULL) {
//    tokens.push_back(renamedFromAttrs.as<string>());
//  }
//
//  string akey = owner->jsName + "." + meta->jsName;
//  string akey2 = owner->swiftName + "." + meta->jsName;
//
//  if (lookupApiNotes(akey) != akey) {
//    tokens.push_back(lookupApiNotes(akey));
//  }
//  else if (lookupApiNotes(akey2) != akey2) {
//    tokens.push_back(lookupApiNotes(akey2));
//  }
//
//  while (tokens[tokens.size() - 1][(tokens[tokens.size() - 1]).size() - 1] == ':') {
//    tokens[tokens.size() - 1].pop_back();
//  }
//
//  return tokens;
//}

//bool Meta::MetaData::populateModuleAttrs(string moduleName)
//{
//  string attributesLookupPath = attrLookupRoot + "/" + moduleName + "-AttributeList.yaml";
//
//  try {
//    attributesNode = YAML::LoadFile(attributesLookupPath);
//  } catch (...) {
//    cerr << "Could not find attribute list for " << moduleName << endl;
//    return false;
//  }
//
//  for (YAML::const_iterator it = attributesNode.begin(); it != attributesNode.end(); ++it) {
//    attributesLookup[it->first.as<string>()] = it->second;
//  }
//
//  return true;
//}

bool MetaData::populateModuleAttrs(string moduleName)
{
  string attributesLookupPath = attrLookupRoot + "/" + moduleName + "-AttributeList.yaml";
  YAML::Node attributesNode;

  try {
    attributesNode = YAML::LoadFile(attributesLookupPath);
  } catch (...) {
    cerr << "Could not find attribute list for " << moduleName << endl;
    return false;
  }
  
  for (YAML::const_iterator it = attributesNode.begin(); it != attributesNode.end(); ++it) {
    attributesLookup[it->first.as<string>()] = it->second;
  }
  
  return true;
}

bool MetaData::populateModule(string moduleName)
{
  string apiNotesPath = sdkRoot + "/System/Library/Frameworks/" +
  moduleName + ".framework/Versions/Current/Headers/" +
  moduleName + ".apinotes";
  
  if (moduleName == "ObjectiveC") {
    apiNotesPath = "/usr/include/objc/ObjectiveC.apinotes";
  }
  
  YAML::Node notes;
  
  try {
    notes = YAML::LoadFile(apiNotesPath);
    //    cout << "Loaded API notes for " << moduleName << endl;
  } catch (...) {
    //    cout << "! Could not find apinotes for " << moduleName << endl;
  }
  
  if (notes["Name"]) {
    for (auto tag : notes["Globals"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Typedefs"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Protocols"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Tags"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Typedefs"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Enumerators"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto tag : notes["Globals"]) {
      if (tag["SwiftName"]) {
        apiNotes[tag["Name"].as<string>()] = tag["SwiftName"].as<string>();
      }
    }
    for (auto cls : notes["Classes"]) {
      if (cls["Name"] && cls["SwiftName"]) {
        apiNotes[cls["Name"].as<string>()] = cls["SwiftName"].as<string>();
      }
      
      if (cls["Name"] && cls["SwiftBridge"]) {
        apiNotes[cls["Name"].as<string>()] = cls["SwiftBridge"].as<string>();
      }
      
      for (auto method : cls["Methods"]) {
        if (cls["Name"] && method["SwiftName"] && method["Selector"]) {
          apiNotes[cls["Name"].as<string>() + "." + method["SwiftName"].as<string>()] = method["Selector"].as<string>();
          apiNotes[cls["Name"].as<string>() + "." + method["Selector"].as<string>()] = method["SwiftName"].as<string>();
        }
      }
      for (auto property : cls["Properties"]) {
        if (cls["Name"] && property["SwiftName"] && property["Name"]) {
          apiNotes[cls["Name"].as<string>() + "." + property["Name"].as<string>()] = property["SwiftName"].as<string>();
        }
      }
    }
  }
  
  populateModuleAttrs(moduleName);
  
  return true;
}

bool MetaData::getUnavailableInSwift(::Meta::Meta* meta, ::Meta::Meta* owner) {
  auto ownerKey = owner->jsName;
  
  if (ownerKey == "NSURL") {
    ownerKey = "URL";
  }
  if (ownerKey == "AffineTransform") {
    ownerKey = "NSAffineTransform";
  }

  auto key = ownerKey + "." + meta->name;

  // attr lookups use the selector (meta->name)
  for (auto attribute: attributesLookup[key]) {
    auto key = attribute.first.as<string>();
    auto value = attribute.second.as<string>();

    if (key == "deprecated") {
      if (value != "100000") {
        return true;
      }
    }

    if (key == "unavailable") {
      return true;
    }
  }

  vector<clang::AvailabilityAttr*> availabilityAttributes = ::Meta::Utils::getAttributes<clang::AvailabilityAttr>(*meta->declaration);

  for (clang::AvailabilityAttr* availability : availabilityAttributes) {
    string platform = availability->getPlatform()->getName().str();

    if (platform != string("macos")) {
      continue;
    }

    if (availability->getUnavailable()) {
      return true;
    }
  }

  // Other edge cases

  // Fixes, "unavailable instance method 'quickLookPreviewableItemsInRanges' was used to satisfy a requirement",
  // even though this method is not marked unavailable in objc nor swift
  if (key == "NSTextView.quickLookPreviewableItemsInRanges:") {
    return true;
  }
  
  if (key == "Process.launchAndReturnError:") {
    return true;
  }

  return false;
}

string MetaData::renamedName(string name, string ownerKey) {
  string key = name;

  if (ownerKey.size()) {
    if (ownerKey == "AffineTransform") {
      ownerKey = "NSAffineTransform";
    }

    key = ownerKey + "." + name;
  }
  
  string newName;
  auto attribute = MetaData::attributesLookup[key]["renamed"];

  if (attribute != NULL) {
    newName = attribute.as<string>();
  }
  else if (!apiNotes[key].empty()) {
    newName = apiNotes[key];
  }
  
  if (newName.empty()) {
    return name;
  }

  std::regex re("^Swift\\.");
  newName = std::regex_replace(newName, re, "");

  // Normalize selector style from:
  // nextEvent(matching:until:inMode:dequeue:)
  // to
  // nextEvent:matching:until:inMode:dequeue:
  std::regex re1("\\(");
  std::regex re2("\\)");
  newName = std::regex_replace(newName, re1, ":");
  newName = std::regex_replace(newName, re2, "");
  
  // Use `create` instead of `init` for initializers,
  // since JSExport doesn't work on `init`
  std::regex ire("^init:");
  newName = std::regex_replace(newName, ire, "create:");
  
  return newName;
}

string MetaData::lookupApiNotes(string type) {
  if (!apiNotes[type].empty()) {
    return apiNotes[type];
  }
  
  return type;
}

string MetaData::dumpDeclComments(::Meta::Meta* meta, ::Meta::Meta* owner) {
  string out;
  
  out += "\n  /**\n";
  out += "    - jsName: " + meta->jsName + "\n";
  out += "    - name: " + meta->name + "\n";
  out += "    - argLabels: " + StringUtils::join(meta->argLabels, ", ") + "\n";
  
  if (meta->is(MetaType::Method)) {
    MethodMeta& method = meta->as<MethodMeta>();
    out += "    - constructorTokens: " + StringUtils::join(method.constructorTokens, ", ") + "\n";
  }
  
  bool matchedAttrLookup = false;
  
  // Attribute lookups use the selector (meta->name)
  for (auto attribute: attributesLookup[owner->jsName + "." + meta->name]) {
    matchedAttrLookup = true;
    out += "    - " + attribute.first.as<string>() + ": " + attribute.second.as<string>() + "\n";
  }
  
  // Try using swiftName instead of jsName in case the latter didn't work
  if (!matchedAttrLookup) {
    for (auto attribute: attributesLookup[owner->jsName + "." + meta->name]) {
      matchedAttrLookup = true;
      out += "    - " + attribute.first.as<string>() + ": " + attribute.second.as<string>() + "\n";
    }
  }
  
  vector<clang::AvailabilityAttr*> availabilityAttributes = Utils::getAttributes<clang::AvailabilityAttr>(*meta->declaration);
  
  for (clang::AvailabilityAttr* availability : availabilityAttributes) {
    if (availability->getPlatform()->getName().str() != string("macos")) {
      continue;
    }
    
    if (!availability->getIntroduced().empty()) {
      out += "    - Introduced: " + MetaFactory::convertVersion(availability->getIntroduced()).to_string() + "\n";
    }
    if (!availability->getDeprecated().empty()) {
      out += "    - Deprecated: " + MetaFactory::convertVersion(availability->getDeprecated()).to_string() + "\n";
    }
    if (!availability->getObsoleted().empty()) {
      out += "    - Obsoleted: " + MetaFactory::convertVersion(availability->getObsoleted()).to_string() + "\n";
    }
    if (!availability->getReplacement().empty()) {
      out += "    - Replacement: " + availability->getReplacement().str() + "\n";
    }
    if (!availability->getMessage().empty()) {
      out += "    - Message: " + availability->getMessage().str() + "\n";
    }
    if (availability->getUnavailable()) {
      out += "    - Unavailable\n";
    }
  }
  
  out += "  */";
  
  return out;
}

}
