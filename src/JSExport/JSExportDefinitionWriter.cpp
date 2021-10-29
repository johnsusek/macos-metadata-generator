#include "TypeScript/DefinitionWriter.h"
#include "JSExportDefinitionWriter.h"
#include "JSExportFormatter.h"
#include "Meta/MetaData.h"
#include "Meta/MetaFactory.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>
#include <regex>
#include "yaml-cpp/yaml.h"
#include <cstdlib>

namespace TypeScript {
using namespace Meta;
using namespace std;

string JSExportDefinitionWriter::outputJSEFolder = "";

static unordered_set<string> hiddenMethods = {
  "alloc",
  "allocWith",
  "allocWithZone"
  "autorelease",
  "conformsToProtocol",
  "copy",
  "mutableCopy",
  "initialize",
  "load",
  "release",
  "retain",
  "new"
  "self",
  "zone",
  "class",
  "subscript"
};

unordered_set<string> JSExportDefinitionWriter::hiddenClasses = {
  "Protocol"
};

static unordered_set<string> anyObjectProps = {
  "owner",
  "delegate",
  "target",
  "item",
  "firstItem",
  "secondItem"
};

static string getTypeParametersStringOrEmpty(const clang::ObjCInterfaceDecl* interfaceDecl)
{
  ostringstream output;

  if (clang::ObjCTypeParamList* typeParameters = interfaceDecl->getTypeParamListAsWritten()) {
    if (typeParameters->size()) {
      output << "JSValue";
    }
  }

  return output.str();
}

static vector<string> getTypeParameterNames(const clang::ObjCInterfaceDecl* interfaceDecl)
{
  vector<string> params;
  if (clang::ObjCTypeParamList* typeParameters = interfaceDecl->getTypeParamListAsWritten()) {
    if (typeParameters->size()) {
      for (unsigned i = 0; i < typeParameters->size(); i++) {
        clang::ObjCTypeParamDecl* typeParam = *(typeParameters->begin() + i);
        params.push_back(typeParam->getNameAsString());
      }
    }
  }
  return params;
}

void getClosedGenericsIfAnyJS(Type& type, vector<Type*>& params)
{
  if (type.is(TypeInterface)) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    for (size_t i = 0; i < interfaceType.typeArguments.size(); i++) {
      getClosedGenericsIfAnyJS(*interfaceType.typeArguments[i], params);
    }
  } else if (type.is(TypeTypeArgument)) {
    TypeArgumentType* typeArg = &type.as<TypeArgumentType>();
    
    if (typeArg->visit(NameRetrieverVisitor::instanceTs) != "") {
      if (find(params.begin(), params.end(), typeArg) == params.end()) {
        params.push_back(typeArg);
      }
    }
  }
}

bool methodHasGenericParams(MethodMeta* method) {
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  const auto parameters = methodDecl.parameters();
  bool hasGenericParams = false;
  
  for (size_t i = 0; i < parameters.size(); i++) {
    const clang::ParmVarDecl parmVar = *parameters[i];
    vector<Type*> paramsGenerics;
    
    getClosedGenericsIfAnyJS(*method->signature[i+1], paramsGenerics);
    
    if (!paramsGenerics.empty()) {
      hasGenericParams = true;
    }
  }
  
  return hasGenericParams;
}

bool JSExportDefinitionWriter::isSubclassOf(string superclass, InterfaceMeta* meta) {
  bool stop = false;
  bool isSuperclass = false;
  auto base = meta->base;

  while (stop == false) {
    if (base && base != NULL && base != nullptr) {
      if (base->jsName == superclass) {
        isSuperclass = true;
        stop = true;
      }
      else {
        base = base->base;
      }
    }
    else {
      stop = true;
    }
  }
  
  return isSuperclass;
}

void JSExportDefinitionWriter::getInheritedMembersRecursive(InterfaceMeta* interface,
                                                            CompoundMemberMap<MethodMeta>* staticMethods,
                                                            CompoundMemberMap<MethodMeta>* instanceMethods,
                                                            CompoundMemberMap<PropertyMeta>* staticProperties,
                                                            CompoundMemberMap<PropertyMeta>* instanceProperties)
{
  auto base = interface->base;
  if (!base) {
    return;
  }
  
  if (staticMethods) {
    for (MethodMeta* method : base->staticMethods) {
      if (staticMethods->find(method->jsName) == staticMethods->end()) {
        staticMethods->emplace(method->jsName, make_pair(base, method));
      }
    }
  }

  if (instanceMethods) {
    for (MethodMeta* method : base->instanceMethods) {
      if (instanceMethods->find(method->jsName) == instanceMethods->end()) {
        instanceMethods->emplace(method->jsName, make_pair(base, method));
      }
    }
  }

  if (staticProperties) {
    for (PropertyMeta* property : base->staticProperties) {
      if (staticProperties->find(property->jsName) == staticProperties->end()) {
        staticProperties->emplace(property->jsName, make_pair(base, property));
      }
    }
  }

  if (instanceProperties) {
    for (PropertyMeta* property : base->instanceProperties) {
      if (instanceProperties->find(property->jsName) == instanceProperties->end()) {
        instanceProperties->emplace(property->jsName, make_pair(base, property));
      }
    }
  }

  // accumulate...
  unordered_set<ProtocolMeta*> protocols;
  for (auto protocol : base->protocols) {
    getProtocolMembersRecursive(protocol, staticMethods, instanceMethods, staticProperties, instanceProperties, protocols);
  }

  getInheritedMembersRecursive(base, staticMethods, instanceMethods, staticProperties, instanceProperties);
}

void JSExportDefinitionWriter::getProtocolMembersRecursive(ProtocolMeta* protocolMeta,
                                                           CompoundMemberMap<MethodMeta>* staticMethods,
                                                           CompoundMemberMap<MethodMeta>* instanceMethods,
                                                           CompoundMemberMap<PropertyMeta>* staticProperties,
                                                           CompoundMemberMap<PropertyMeta>* instanceProperties,
                                                           unordered_set<ProtocolMeta*>& visitedProtocols)
{
  visitedProtocols.insert(protocolMeta);

  if (staticMethods) {
    for (MethodMeta* method : protocolMeta->staticMethods) {
      if (staticMethods->find(method->name) == staticMethods->end()) {
        staticMethods->emplace(method->name, make_pair(protocolMeta, method));
      }
    }
  }

  if (instanceMethods) {
    for (MethodMeta* method : protocolMeta->instanceMethods) {
      if (instanceMethods->find(method->jsName) == instanceMethods->end()) {
        instanceMethods->emplace(method->jsName, make_pair(protocolMeta, method));
      }
    }
  }

  if (staticProperties) {
    for (PropertyMeta* property : protocolMeta->staticProperties) {
      if (staticProperties->find(property->jsName) == staticProperties->end()) {
        staticProperties->emplace(property->jsName, make_pair(protocolMeta, property));
      }
    }
  }

  if (instanceProperties) {
    for (PropertyMeta* property : protocolMeta->instanceProperties) {
      if (instanceProperties->find(property->jsName) == instanceProperties->end()) {
        instanceProperties->emplace(property->jsName, make_pair(protocolMeta, property));
      }
    }
  }

  for (ProtocolMeta* protocol : protocolMeta->protocols) {
    getProtocolMembersRecursive(protocol, staticMethods, instanceMethods, staticProperties, instanceProperties, visitedProtocols);
  }
}

string JSExportDefinitionWriter::getMethodReturnType(MethodMeta* meta, BaseClassMeta* owner, size_t numArgs, const bool skipGenerics) {
  string output = "";
  vector<Type*> paramsGenerics;
  vector<string> ownerGenerics;

  if (owner->is(Interface)) {
    ownerGenerics = getTypeParameterNames(clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration));
  }

  for (size_t i = 0; i < numArgs; i++) {
    if (meta->signature.size() > i + 1) {
      getClosedGenericsIfAnyJS(*meta->signature[i+1], paramsGenerics);
    }
  }

  if (!paramsGenerics.empty()) {
    for (size_t i = 0; i < paramsGenerics.size(); i++) {
      string name = paramsGenerics[i]->visit(NameRetrieverVisitor::instanceTs);
      if (find(ownerGenerics.begin(), ownerGenerics.end(), name) == ownerGenerics.end())
      {
        paramsGenerics.erase(paramsGenerics.begin() + i);
        i--;
      }
    }
  }

  const Type* retType = meta->signature[0];
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(meta->declaration);

  if (!methodDecl.isInstanceMethod() && owner->is(MetaType::Interface)) {
    if ((retType->is(TypeInstancetype) || DefinitionWriter::hasClosedGenerics(*retType)) && !skipGenerics) {
      output += getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration));
    } else if (!paramsGenerics.empty()) {
      output += "<";
      for (size_t i = 0; i < paramsGenerics.size(); i++) {
        auto name = paramsGenerics[i]->visit(NameRetrieverVisitor::instanceTs);
        output += JSExportFormatter::current.nameForJSExport(name);
        if (i < paramsGenerics.size() - 1) {
          output += ", ";
        }
      }
      output += ">";
    }
  }

  return "";
}

// MARK: - Write Method

string JSExportDefinitionWriter::writeMethod(MethodMeta* method, BaseClassMeta* owner, string keyword, string metaJsName)
{
  ostringstream output;
  
  if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
    return output.str();
  }
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  string retTypeString = JSExportFormatter::current.formatType(*method->signature[0], methodDecl.getReturnType());

  bool unavailableInSwift = MetaData::getUnavailableInSwift(method, owner);

  if (unavailableInSwift && !method->isRenamed) {
    output << "// unavailableInSwift ";
  }
  
  output << "@objc ";
  
  // only add if fn renamed
  if (method->isRenamed && !method->isInit() && !methodHasGenericParams(method)) {
    string selector = methodDecl.getSelector().getAsString();
    output << "(" + selector + ") ";
  }
  
  output << method->availableString();

  if (!keyword.empty()) {
    output << keyword + " ";
  }
  
  output << "func ";
  
  output << JSExportFormatter::current.sanitizeIdentifierForSwift(method->jsName);
  
  auto methodParams = JSExportFormatter::current.getMethodParams(method, owner);
  
  // Don't have clang::Qualifiers::OCL_Autoreleasing?
  static unordered_set<string> autoreleasingMethods = {
    "getResourceValue:forKey:error:",
    "getPromisedItemResourceValue:forKey:error:",
    "smartInsertForString:replacingRange:beforeString:afterString:",
  };

  if (autoreleasingMethods.find(method->getSelector()) != autoreleasingMethods.end()) {
    std::regex re("UnsafeMutablePointer");
    methodParams = std::regex_replace(methodParams, re, "AutoreleasingUnsafeMutablePointer");
  }
  
  output << methodParams;

  if (retTypeString != "Void" && retTypeString != "") {
    output << " -> " + retTypeString;
    output << JSExportFormatter::current.getTypeNullability(method, owner);    
  }
  
  string out = output.str();
  
  regex re1(".*JSValue.*");
  bool returnsJSValue = regex_match(out, re1);

  if (returnsJSValue || retTypeString == "JSValue") {
    if (!method->isInit()) {
      return "// jsvalue - " + out;
    }
  }
  if (method->getFlags(MetaFlags::MethodHasErrorOutParameter) && !method->isInit()) {
    return "// throws - " + out;
  }

  return out;
}

string JSExportDefinitionWriter::writeMethod(CompoundMemberMap<MethodMeta>::value_type& methodPair, BaseClassMeta* owner, const unordered_set<ProtocolMeta*>& protocols, string keyword, string metaJsName)
{
  string output;
  MethodMeta* method = methodPair.second.second;

  if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
    return output;
  }
  
  BaseClassMeta* memberOwner = methodPair.second.first;
  
  bool isOwnMethod = memberOwner == owner;
  bool implementsProtocol = protocols.find(static_cast<ProtocolMeta*>(memberOwner)) != protocols.end();
  bool returnsInstanceType = method->signature[0]->is(TypeInstancetype);
  
  if (isOwnMethod || implementsProtocol || returnsInstanceType) {
    output = writeMethod(method, owner, keyword, metaJsName);
  }
  
  return output;
}

// MARK: - Write Property

void JSExportDefinitionWriter::writeProperty(PropertyMeta* meta, BaseClassMeta* owner, InterfaceMeta* target, CompoundMemberMap<PropertyMeta> baseClassProperties)
{
  if (hiddenMethods.find(meta->jsName) != hiddenMethods.end()) {
    return;
  }
  
  string propValue = writeProperty(meta, target);
  
  regex re(".*JSValue.*");
  bool returnsJSValue = regex_match(propValue, re);

  if (returnsJSValue) {
    _buffer << "// jsvalue ";
  }
  
  _buffer << _docSet.getCommentFor(meta, owner).toString("  ");
  _buffer << "  ";
  
  const clang::ObjCPropertyDecl& propDecl = *clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration);
  
  _buffer << "@objc ";
  
  _buffer << meta->availableString();
  
  if (propDecl.isClassProperty()) {
    _buffer << "static ";
  }
  
  _buffer << propValue;
  
  _buffer << endl;
}

// MARK: - Write Extension

const char * viewOverrides = R"__literal(
  public var draw: JSValue?
  
  public override func draw(_ dirtyRect: NSRect) {
    super.draw(dirtyRect)
    drawOverride(dirtyRect)
  })__literal";

void JSExportDefinitionWriter::writeExtension(string protocolName, InterfaceMeta* meta, CompoundMemberMap<MethodMeta>* staticMethods, CompoundMemberMap<MethodMeta>* instanceMethods) {
  bool isViewSubclass = isSubclassOf("NSView", meta);
  
  if (isViewSubclass) {
    string viewName = meta->jsName.substr(2);
    
    _buffer << "@objc protocol " + viewName + "Exports: JSExport";
    
    if (meta->base) {
      _buffer << ", " << meta->base->jsName << "Exports";
    }
    
    _buffer << " {\n";
    
    if (!staticMethods->empty()) {
      _buffer << "  // Static Methods\n";
      
      for (auto& methodPair : *staticMethods) {
        MethodMeta* method = methodPair.second.second;
        BaseClassMeta* owner = methodPair.second.first;

        if (!method->isInit()) {
          continue;
        }
        
        string output = writeMethod(methodPair, meta, {}, "static", meta->jsName);
        
        if (output.size()) {
          _buffer << MetaData::dumpDeclComments(method, meta) << endl;
          _buffer << _docSet.getCommentFor(method, owner).toString("");
          _buffer << "  " << output << endl;
        }
      }
    }

    _buffer << "}\n\n";

    writeClass(meta, staticMethods, instanceMethods);
  }
  
  _buffer << "extension " << protocolName << ": " << protocolName << "Exports {\n";
  
  for (auto& methodPair: *staticMethods) {
    MethodMeta* method = methodPair.second.second;

    if (method->isInit()) {
      BaseClassMeta* owner = methodPair.second.first;
      writeCreate(method, owner, true);
    }
  }
  
  for (auto& methodPair: *instanceMethods) {
    MethodMeta* method = methodPair.second.second;

    if (method->isInit()) {
      BaseClassMeta* owner = methodPair.second.first;
      writeCreate(method, owner);
    }
  }

  _buffer << "}\n";
}

void JSExportDefinitionWriter::writeCreate(MethodMeta* method, BaseClassMeta* owner, bool isStatic) {
  if (method->constructorTokens.empty() && method->argLabels.empty()) {
    // Empty creates are filtered out earlier, but a few classes
    // have empty inits w/ a different name
    if (method->name == "fieldEditor" || method->name == "baseUnit") {
//      cout << "Skipping empty non-`init` constructor: " << method->name << endl;
      return;
    }
  }
  
  auto methodImplParams = JSExportFormatter::current.getMethodParams(method, owner, JSExportFormatter::ParamCallType::Implementation);
  auto methodCallParams = JSExportFormatter::current.getMethodParams(method, owner, JSExportFormatter::ParamCallType::Call);
  
  _buffer << "  @objc public ";
  
  if (owner->is(MetaType::Interface)) {
    auto interface = &owner->as<InterfaceMeta>();
    auto base = interface->base;
    if (base) {
      for (MethodMeta* baseMethod : base->staticMethods) {
        if (baseMethod->name == method->name) {
          _buffer << "override ";
          break;
        }
      }
    }
  }
  
  _buffer << "static func " << method->jsName << methodImplParams;
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  string retTypeString = JSExportFormatter::current.formatType(*method->signature[0], methodDecl.getReturnType());

  _buffer << " -> " + retTypeString;
  
  string typeNullabilityStr = JSExportFormatter::current.getTypeNullability(method, owner);
  _buffer << typeNullabilityStr;
  
  _buffer << " {\n";

  _buffer << "    return ";

  if (method->getFlags(MetaFlags::MethodHasErrorOutParameter)) {
    _buffer << "try? ";
  }
  
  _buffer << "self.";

  string callName = method->jsName;
  
  if (callName.substr(0, 6) == "create") {
    callName = "init";
  }

  _buffer << callName;
  
  _buffer << methodCallParams << endl;
  
  _buffer << "  }\n\n";
}

void JSExportDefinitionWriter::writeClass(InterfaceMeta* meta, CompoundMemberMap<MethodMeta>* staticMethods, CompoundMemberMap<MethodMeta>* instanceMethods) {
  string viewName = meta->jsName.substr(2);

  _buffer << "@objc(" << viewName << ") public class " + viewName + ": " + meta->jsName;
  _buffer << ", " << viewName << "Exports, JSOverridableView {";
  _buffer << viewOverrides << endl << endl;
  _buffer << "}\n\n";
}

void JSExportDefinitionWriter::writeProto(ProtocolMeta* meta) {
  _buffer << "@objc(" << meta->jsName << ") protocol " << meta->jsName << "Exports: JSExport";
  
  map<string, PropertyMeta*> conformedProtocolsProperties;
  map<string, MethodMeta*> conformedProtocolsMethods;
  
  if (meta->protocols.size()) {
    _buffer << ", ";
    
    for (size_t i = 0; i < meta->protocols.size(); i++) {
      transform(
                meta->protocols[i]->instanceProperties.begin(),
                meta->protocols[i]->instanceProperties.end(),
                inserter(conformedProtocolsProperties,
                         conformedProtocolsProperties.end()
                         ),
                []
                (PropertyMeta* propertyMeta) {
        return make_pair(propertyMeta->name, propertyMeta);
      });
      transform(
                meta->protocols[i]->instanceMethods.begin(),
                meta->protocols[i]->instanceMethods.end(),
                inserter(conformedProtocolsMethods,
                         conformedProtocolsMethods.end()
                         ),
                []
                (MethodMeta* methodMeta) {
        return make_pair(methodMeta->name, methodMeta);
      });
      
      _buffer << meta->protocols[i]->jsName + "Exports";
      
      if (i < meta->protocols.size() - 1) {
        _buffer << ", ";
      }
    }
  }
  
  _buffer << " {" << endl;
}

void JSExportDefinitionWriter::writeProto(string protocolName, InterfaceMeta* meta) {
  _buffer << "@objc(" << protocolName << ") protocol " << protocolName << "Exports: JSExport";
  
  if (meta->base) {
    _buffer << ", " << meta->base->jsName << "Exports";
  }
  
  _buffer << " {" << endl;
}

string JSExportDefinitionWriter::writeProperty(PropertyMeta* meta, BaseClassMeta* owner)
{
  ostringstream output;
  
  string name = meta->jsName;
  string originalName;
  
  output << "var ";
  
  output << JSExportFormatter::current.sanitizeIdentifierForSwift(name);
  
  bool ignorePointerType = false;
  
  if (owner->jsName == "Error") {
    // domain wants `String` instead of `NSErrorDomain`
    // userInfo wants `String` instead of `NSError.UserInfoKey`
    if (name == "domain" || name == "userInfo") {
      ignorePointerType = true;
    }
  }
  
  auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration);
  auto& first = *meta->getter->signature[0];
  string returnType = JSExportFormatter::current.formatType(first, decl->getType(), ignorePointerType);

  //
  // Manual fixes for property return types in the context of a JSExport
  //
  
  // TODO: Better way to detect Any vs AnyObject
  if (returnType == "Any") {
//    auto qualTypeStr = decl->getType().getAsString();

    if (anyObjectProps.find(name) != anyObjectProps.end()
        && owner->jsName != "NSDraggingItem") {
      returnType = "AnyObject";
//      cout << name << " - AnyObject: " << qualTypeStr << endl;
    }
    else {
//      cout << name << " - Any: " << qualTypeStr << endl;
    }
  }
  else if (name == "floatValue") {
    returnType = "Float";
  }
  else if (name == "intValue") {
    returnType = "Int32";
  }
  else if (returnType == "NSLayoutAnchor") {
    returnType = "JSValue";
  }
  
  if (DefinitionWriter::hasClosedGenerics(first)) {
    output << "";
  }

  // Property return type
  output << ": " << returnType;

  bool isNullable = false;
  
  if (JSExportFormatter::nonNullable.find(returnType) == JSExportFormatter::nonNullable.end()) {
    isNullable = true;
  }

  if (returnType == "NSUserInterfaceItemIdentifier") {
    if (owner->jsName == "NSTableColumn" || owner->jsName == "NSLayoutGuide") {
      // Nullable in proto but not impl? How to check for this?
      isNullable = false;
    }
  }
  
  if (isNullable) {
    output << JSExportFormatter::current.getNullabilitySymbol(meta, owner);
  }
  
  output << " {";

  if (meta->getter) {
    output << " @objc";
    if (meta->getter->name != meta->jsName) {
      output << " (" + meta->getter->name + ")";
    }
    output << " get";
  }

  if (meta->setter) {
    if (meta->setter->name.empty()) {
      output << " @objc set";
    }
    else {
      output << " @objc (" << meta->setter->name << ") set";
    }
  }

  output << " }";

  return output.str();
}

// MARK: - Visit Protocol

void JSExportDefinitionWriter::visit(ProtocolMeta* meta)
{
  string metaName = meta->jsName;
  
  _buffer << "\n// Protocol \n";
  
  this->writeProto(meta);
  
  for (PropertyMeta* property : meta->instanceProperties) {
    string output = writeProperty(property, meta);
    if (output.size()) {
      regex re(".*JSValue.*");
      bool returnsJSValue = regex_match(output, re);
      
      if (returnsJSValue) {
        _buffer << "// jsvalue ";
      }

      _buffer << "  " << output << endl;
    }
  }
  
  for (MethodMeta* method : meta->instanceMethods) {
    if (hiddenMethods.find(method->jsName) != hiddenMethods.end() ) {
      continue;
    }
    
    string output = writeMethod(method, meta);
    
    if (output.size()) {
      _buffer << "  " << output << endl;
    }
  }
  
  _buffer << "}" << endl << endl;
}

// MARK: - Visit Interface

void JSExportDefinitionWriter::visit(InterfaceMeta* meta)
{
  if (meta->jsName == "NSDistantObjectRequest") {
    // Unavailable classes
    return;
  }
  
  CompoundMemberMap<MethodMeta> compoundStaticMethods;
  static map<string, bool> addedConstructors = {};

  for (MethodMeta* method : meta->staticMethods) {
    if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
      continue;
    }

    if (method->jsName == "errorWithDomain") {
      continue;
    }
    
    if (method->hasParamOfType("NSInvocation")) {
      // NSInvocation is totally unsupported in Swift
      continue;
    }
    
    if (method->jsName == "create") {
      // Skip empty constructors since bridged NSObject includes this
      if (method->argLabels.empty() || (method->argLabels.size() == 2 && method->argLabels[0] == "target" && method->argLabels[1] == "action")) {
        continue;
      }
      
      // Add constructor name to list so we don't write dupes later
      string compareName = method->jsName + "With";
      
      for (size_t i = 0; i < method->argLabels.size(); i++) {
        auto argLabel = method->argLabels[i];
        argLabel[0] = toupper(argLabel[0]);
        compareName += argLabel;
      }
      
      if (addedConstructors[compareName]) {
        continue;
      }
      
      addedConstructors[compareName] = true;
    }
    
    compoundStaticMethods.emplace(method->name, make_pair(meta, method));
  }
  
  CompoundMemberMap<MethodMeta> compoundInstanceMethods;
  for (MethodMeta* method : meta->instanceMethods) {
    if (method->jsName == "createWithRoundingMode") {
      continue;
    }
    
    if (method->jsName == "createWithLeftExpression") {
      continue;
    }
    
    if (method->jsName == "createWithDescriptorType") {
      continue;
    }
    
    if (method->jsName == "createByResolvingBookmarkData") {
      continue;
    }
    
    if (addedConstructors[method->jsName]) {
      continue;
    }
    else if (!method->argLabels.empty()) {
      // Normalize
      // initWithBytes:objCType:
      // vs
      // init:bytes:objCType:
      // for check
      string firstArg = method->argLabels[0];
      firstArg[0] = toupper(firstArg[0]);

      if (addedConstructors[method->jsName + firstArg]) {
        continue;
      }
    }
    
    bool isSetterForProperty = false;
    
    for (PropertyMeta* property : meta->instanceProperties) {
      if (property->setter && method->jsName == property->setter->jsName) {
        isSetterForProperty = true;
        break;
      }
    }

    if (isSetterForProperty) {
      continue;
    }
    
    compoundInstanceMethods.emplace(method->name, make_pair(meta, method));
  }
  
  CompoundMemberMap<PropertyMeta> baseClassInstanceProperties;
  CompoundMemberMap<PropertyMeta> ownInstanceProperties;
  
  for (PropertyMeta* property : meta->instanceProperties) {
    if (ownInstanceProperties.find(property->name) != ownInstanceProperties.end()) {
      cout << "Skipping property with duplicated name: `" << property->name << "`" << endl;
      continue;
    }
    
    auto interface = &meta->as<InterfaceMeta>();

    if (isSubclassOf("NSControl", interface) &&
        interface->nameExistsInSuperclass(property->jsName, Method)) {
      cout << "Skipping property that exists as method in " << interface->jsName << " superclass: `" << property->jsName << "`" << endl;
      continue;
    }
    
    if (property->name == "selectedTag" && isSubclassOf("NSControl", interface)) {
      continue;
    }
    if (property->name == "selectedCell" && isSubclassOf("NSControl", interface)) {
      continue;
    }

    auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(property->declaration);
    auto& first = *property->getter->signature[0];
    string returnType = JSExportFormatter::current.formatType(first, decl->getType());
    
    if (returnType == "NSInvocation") {
      // NSInvocation is totally unsupported in Swift
      continue;
    }

    ownInstanceProperties.emplace(property->name, make_pair(meta, property));
  }
  
  CompoundMemberMap<PropertyMeta> baseClassStaticProperties;
  CompoundMemberMap<PropertyMeta> ownStaticProperties;
  
  for (PropertyMeta* property : meta->staticProperties) {
    if (ownStaticProperties.find(property->name) == ownStaticProperties.end()) {
      ownStaticProperties.emplace(property->name, make_pair(meta, property));
    }
  }
  
  unordered_set<ProtocolMeta*> inheritedProtocols;
  CompoundMemberMap<MethodMeta> inheritedStaticMethods;
  
  getInheritedMembersRecursive(meta, &inheritedStaticMethods, nullptr, nullptr, nullptr);
  
  for (auto& methodPair : inheritedStaticMethods) {
    MethodMeta* method = methodPair.second.second;
    
    if (compoundStaticMethods.find(method->name) != compoundStaticMethods.end()) {
      continue;
    }
    
    if (method->jsName == "create") {
//      cerr << "Skipping inherited create " << method->name << " in " << meta->jsName << " from " << meta->base->jsName << endl;
      continue;
    }
    
    compoundStaticMethods.emplace(methodPair);
  }
  
  if (compoundStaticMethods.empty() && compoundInstanceMethods.empty() &&
      ownStaticProperties.empty() && ownInstanceProperties.empty()) {
    return;
  }
  
  string metaName = meta->jsName;
  CompoundMemberMap<PropertyMeta> protocolInheritedStaticProperties;
  CompoundMemberMap<PropertyMeta> protocolInheritedInstanceProperties;
  unordered_set<ProtocolMeta*> protocols;

  // For bridging protocols, we just populate our class with
  // the methods/properties of the protocol and they get written
  // out with everything else
  if (meta->protocols.size()) {
    for (size_t i = 0; i < meta->protocols.size(); i++) {
      auto proto = *meta->protocols[i];
      
      if (proto.jsName == metaName) {
        cout << "Skipping proto with same name as interface: " << proto.jsName << endl;
        continue;
      }
      
      // TODO: Temp during dev
      if (proto.jsName != "NSUserInterfaceItemIdentification" && proto.jsName != "NSObjectProtocol") {
        continue;
      }
      
      cout << meta->jsName << " proto.jsName: " << proto.jsName << endl;

      // fill out our collections of methods/properties from protocol
      getProtocolMembersRecursive(meta->protocols[i],
                                  &compoundStaticMethods,
                                  &compoundInstanceMethods,
                                  &protocolInheritedStaticProperties,
                                  &protocolInheritedInstanceProperties, protocols);
    }
  }
  
  unordered_set<ProtocolMeta*> immediateProtocols;
  
  for (auto protocol : protocols) {
    if (inheritedProtocols.find(protocol) == inheritedProtocols.end()) {
      immediateProtocols.insert(protocol);
    }
  }

  _buffer << "\n// Interface \n";
  
  _buffer << MetaData::dumpDeclComments(meta, meta) << endl << endl;
  _buffer << _docSet.getCommentFor(meta).toString("");
  
  string protocolName = meta->jsName;
  
  // Fix "extension of protocol 'Error' cannot have an inheritance clause"
  if (meta->jsName == "Error" || meta->jsName == "URL" || meta->jsName == "AffineTransform") {
    protocolName = meta->name;
  }
  
  this->writeProto(protocolName, meta);
  
  if (!compoundStaticMethods.empty()) {
    _buffer << "  // Static Methods\n";

    for (auto& methodPair : compoundStaticMethods) {
      MethodMeta* method = methodPair.second.second;
      BaseClassMeta* owner = methodPair.second.first;
      
      // Don't write static method w/ same name as static property
      if (ownStaticProperties.find(methodPair.first) != ownStaticProperties.end()) {
        continue;
      }
      
      // Don't write static method w/ same name as instance property
      // Fixes e.g. isSeparatorItem in NSMenuItem
      if (ownInstanceProperties.find(method->name) != ownInstanceProperties.end()) {
        cout << "Skipping method `" << methodPair.first << "` in " << protocolName << " because instance property with same name exists" << endl;
        continue;
      }
      
      string output = writeMethod(methodPair, meta, immediateProtocols, "static", metaName);
      
      if (output.size()) {
        _buffer << MetaData::dumpDeclComments(method, meta) << endl;
        _buffer << _docSet.getCommentFor(method, owner).toString("");
        _buffer << "  " << output << endl;
      }
    }
  }

  if (!protocolInheritedStaticProperties.empty()) {
    _buffer << "\n  // Protocol Inherited Static Properties\n";
    
    for (auto& propertyPair : protocolInheritedStaticProperties) {
      BaseClassMeta* owner = propertyPair.second.first;
      PropertyMeta* propertyMeta = propertyPair.second.second;
      
      if (propertyMeta->unavailable) {
        _buffer << "// ";
      }
      
      this->writeProperty(propertyMeta, owner, meta, baseClassStaticProperties);
    }
  }
  
  if (!ownStaticProperties.empty()) {
    _buffer << "\n  // Own Static Properties\n";

    for (auto& propertyPair : ownStaticProperties) {
      BaseClassMeta* owner = propertyPair.second.first;
      PropertyMeta* propertyMeta = propertyPair.second.second;
      
      // We only want our own static properties here.
      if (owner != meta) {
        continue;
      }
      
      _buffer << MetaData::dumpDeclComments(propertyMeta, meta) << endl;
      
      if (propertyMeta->unavailable) {
        _buffer << "  // ";
      }
      
      this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
    }
  }
  
  if (!compoundInstanceMethods.empty()) {
    _buffer << "\n  // Instance Methods\n";

    for (auto& methodPair : compoundInstanceMethods) {
      MethodMeta* method = methodPair.second.second;
      
      if (ownInstanceProperties.find(methodPair.first) != ownInstanceProperties.end()) {
        cout << "Skipping method `" << method->jsName << "` because property with same name exists " << endl;
        continue;
      }
      
      string output = writeMethod(methodPair, meta, immediateProtocols, method->isInit() ? "static" : "", metaName);
      
      if (output.size()) {
        _buffer << MetaData::dumpDeclComments(method, meta) << endl;
        _buffer << "  ";
        _buffer << _docSet.getCommentFor(methodPair.second.second, methodPair.second.first).toString("  ");
        _buffer << output << endl;
      }
    }
  }
  
  if (!protocolInheritedInstanceProperties.empty()) {
    _buffer << "\n  // Protocol Inherited Instance Properties\n";

    for (auto& propertyPair : protocolInheritedInstanceProperties) {
      BaseClassMeta* owner = propertyPair.second.first;
      PropertyMeta* propertyMeta = propertyPair.second.second;
      
      bool isDuplicated = ownInstanceProperties.find(propertyMeta->jsName) != ownInstanceProperties.end();
      if (immediateProtocols.find(reinterpret_cast<ProtocolMeta*>(owner)) != immediateProtocols.end() && !isDuplicated) {
        _buffer << MetaData::dumpDeclComments(propertyMeta, meta) << endl;
        
        if (propertyMeta->unavailable) {
          _buffer << "// ";
        }
        
        this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
      }
    }
  }
   
  if (!ownInstanceProperties.empty()) {
    _buffer << "\n  // Own Instance Properties\n";

    for (auto& propertyPair : ownInstanceProperties) {
      BaseClassMeta* owner = propertyPair.second.first;
      PropertyMeta* propertyMeta = propertyPair.second.second;
      
      if (owner != meta) {
        continue;
      }
      
      _buffer << MetaData::dumpDeclComments(propertyMeta, meta) << endl;
      
      if (propertyMeta->unavailable) {
        _buffer << "// unavailable ";
      }
      
      this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
    }
  }
  
  _buffer << "}\n\n"; // close writeProto

  this->writeExtension(protocolName, meta, &compoundStaticMethods, &compoundInstanceMethods);
}

void JSExportDefinitionWriter::visit(CategoryMeta* meta)
{
}

void JSExportDefinitionWriter::visit(FunctionMeta* meta)
{
}

void JSExportDefinitionWriter::visit(StructMeta* meta)
{
}

void JSExportDefinitionWriter::visit(UnionMeta* meta)
{
}

void JSExportDefinitionWriter::visit(EnumMeta* meta)
{
}

void JSExportDefinitionWriter::visit(VarMeta* meta)
{
}

void JSExportDefinitionWriter::visit(MethodMeta* meta)
{
}

void JSExportDefinitionWriter::visit(PropertyMeta* meta)
{
}

void JSExportDefinitionWriter::visit(EnumConstantMeta* meta)
{
}

string JSExportDefinitionWriter::write()
{
  _buffer.clear();
  _importedModules.clear();

  for (::Meta::Meta* meta : _module.second) {
    meta->visit(this);
    
    bool isNotHidden = hiddenClasses.find(meta->jsName) == hiddenClasses.end();
    
    if ((meta->is(MetaType::Interface) || meta->is(MetaType::Protocol)) && isNotHidden) {
      string filename = meta->jsName + ".swift";
      
      if (meta->is(MetaType::Interface) && isSubclassOf("NSView", &meta->as<InterfaceMeta>())) {
        regex frameworkPrefixes("^NS");
        string noprefixName = regex_replace(meta->jsName, frameworkPrefixes, "");
        filename = noprefixName + ".swift";
      }
      
      writeJSExport(filename, meta, _module.first->Name);
    }

    _buffer.str("");
    _buffer.clear();
  }

  return "";
}

void JSExportDefinitionWriter::writeJSExport(string filename, ::Meta::Meta* meta, string frameworkName)
{
  auto buffer = _buffer.str();
  
  if (buffer.empty() || buffer == "\n" || meta->jsName[0] == '_') {
    return;
  }
  
  string jsPath = JSExportDefinitionWriter::outputJSEFolder + "/" + frameworkName + "/";
  if (meta->is(MetaType::Protocol)) {
    jsPath += "protocols/";
  }
  
  string mkdirCmd = "mkdir -p '" + jsPath + "'";
  const size_t mkdirReturn = system(mkdirCmd.c_str());
  
  if (mkdirReturn < 0)  {
    cout << "Error creating directory using command: " << mkdirCmd << endl;
    return;
  }

  error_code writeError;

  llvm::raw_fd_ostream jsFile(jsPath + filename, writeError, llvm::sys::fs::F_Text);
  
  if (writeError) {
    cout << writeError.message();
    return;
  }

  jsFile << "import AppKit\nimport JavaScriptCore\n";
  
  // TODO: dynamically add these based on user's framework prefs in their xcodegen file
  // this is just a really lazy way to get the JSEs to all compile, at the expense of memory usage
  jsFile << "import Quartz\nimport AVKit\nimport CoreImage\nimport CoreGraphics\n";
  
  jsFile << "import " << frameworkName << "\n";
  
  jsFile << buffer;
  
  jsFile.close();

//  cout << "Wrote " << frameworkName + "/" + filename << endl;
}
}

