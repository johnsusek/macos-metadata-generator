#include "TypeScript/DefinitionWriter.h"
#include "JSExportDefinitionWriter.h"
#include "Meta/MetaEntities.h"
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
  "createWithCoder",
  "subscript",
  "errorWithDomain",
  "createWithRoundingMode",
  "createWithLeftExpression",
  "createWithDescriptorType",
  "createByResolvingBookmarkData"
};

static unordered_set<string> hiddenNames = {
  "registerClass:forItemWithIdentifier:",
  "registerClass:forSupplementaryViewOfKind:withIdentifier:",
  "URLByResolvingAliasFileAtURL:options:error:",
  "URLByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:",
  "launchAppWithBundleIdentifier:options:additionalEventParamDescriptor:launchIdentifier:",
  "getFileSystemInfoForPath:isRemovable:isWritable:isUnmountable:description:type:",
  "dataTaskWithRequest:completionHandler:",
  "downloadTaskWithRequest:completionHandler:",
  "openURLs:withAppBundleIdentifier:options:additionalEventParamDescriptor:launchIdentifiers:",
  "getTasksWithCompletionHandler:",
  "getObjectValue:forString:errorDescription:"
};

unordered_set<string> JSExportDefinitionWriter::writeInstanceInits = {
  "NSWindow",
  "NSImage",
  "NSString"
};

unordered_set<string> JSExportDefinitionWriter::writeMethodImpls = {
  "NSLayoutAnchor",
  "NSLayoutConstraint",
  "URLSessionWebSocketTask",
  "URLSession"
};

// Swift overlays certain framework classes, but when you try to extend them you get
// the error "extension of protocol 'Error' cannot have an inheritance clause",
// using the original name fixes this
unordered_set<string> JSExportDefinitionWriter::overlaidClasses = {
  "Error",
  "URL",
  "URLQueryItem",
  "URLComponents",
  "URLCredential",
  "URLProtectionSpace",
  "URLRequest",
  "URLSessionTaskTransactionMetrics",
  "URLSessionWebSocketMessage"
};

unordered_set<string> JSExportDefinitionWriter::hiddenClasses = {
  "Protocol",
  "NSString",
  "NSData"
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
    auto& pType = *method->signature[i+1];
    if (pType.is(TypeBlock)) {
      return true;
    }

    getClosedGenericsIfAnyJS(pType, paramsGenerics);
    
    if (!paramsGenerics.empty()) {
      return true;
    }
  }
  
  return hasGenericParams;
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
    if ((retType->is(TypeInstancetype) || retType->hasClosedGenerics()) && !skipGenerics) {
      output += getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration));
    } else if (!paramsGenerics.empty()) {
      output += "<";
      for (size_t i = 0; i < paramsGenerics.size(); i++) {
        auto name = paramsGenerics[i]->visit(NameRetrieverVisitor::instanceTs);
        output += ::Meta::Type::nameForJSExport(name);
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
  auto& typeArg = *method->signature[0];

  string retTypeString = Type::formatType(typeArg, methodDecl.getReturnType());

  if (method->name == "URLFromPasteboard:" ||
      method->name == "URLWithDataRepresentation:relativeToURL:") {
    retTypeString = "NSURL";
  }
  
  bool unavailableInSwift = method->getUnavailableInSwift(owner);

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
  
  string methodParams;
  
  if (method->isInit()) {
    output << ::Meta::sanitizeIdentifierForSwift(method->builtName());
    methodParams = method->getParamsAsString(owner, ParamCallType::Implementation);
  }
  else if (method->getParamsAsString(owner).find("JSValue") != string::npos) {
    output << ::Meta::sanitizeIdentifierForSwift(method->jsName);
    methodParams = method->getParamsAsString(owner);
  }
  else {
    output << ::Meta::sanitizeIdentifierForSwift(method->jsName);
    methodParams = method->getParamsAsString(owner);
  }
  
  // Don't have clang::Qualifiers::OCL_Autoreleasing?
  static unordered_set<string> autoreleasingMethods = {
    "getObjectValue:forString:errorDescription:",
    "isPartialStringValid:proposedSelectedRange:originalString:originalSelectedRange:errorDescription:",
    "isPartialStringValid:newEditingString:errorDescription:",
    "getResourceValue:forKey:error:",
    "getPromisedItemResourceValue:forKey:error:",
    "smartInsertForString:replacingRange:beforeString:afterString:",
    "getFileSystemInfoForPath:isRemovable:isWritable:isUnmountable:description:type:",
    "getInfoForFile:application:type:"
  };

  if (autoreleasingMethods.find(method->getSelector()) != autoreleasingMethods.end()) {
    std::regex re("UnsafeMutablePointer");
    methodParams = std::regex_replace(methodParams, re, "AutoreleasingUnsafeMutablePointer");
  }
  
  output << methodParams;

  if (retTypeString != "Void" && retTypeString != "") {
    output << " -> " + retTypeString;
    output << method->getTypeNullability(owner);
  }
  
  string out = output.str();
  
//  regex re1(".*JSValue.*");
//  bool returnsJSValue = regex_match(out, re1);
//
//  if (returnsJSValue || retTypeString == "JSValue") {
//    if (!method->isInit()) {
//      return "// jsvalue - " + out;
//    }
//  }
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
  
//  bool isOwnMethod = memberOwner == owner;
  bool implementsProtocol = protocols.find(static_cast<ProtocolMeta*>(memberOwner)) != protocols.end();
  bool returnsInstanceType = method->signature[0]->is(TypeInstancetype);
  bool isNSObject = memberOwner->name == "NSObject";
  
  if (!isNSObject || implementsProtocol || returnsInstanceType) {
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
  _buffer << "extension " << protocolName << ": " << protocolName << "Exports {\n";
  
  for (auto& methodPair: *staticMethods) {
    MethodMeta* method = methodPair.second.second;

    if (method->isInit()) {
      BaseClassMeta* owner = methodPair.second.first;
      
      if (protocolName == "FileHandle") {
        continue;
      }
      
      if (protocolName == "AffineTransform") {
        continue;
      }
      
      writeCreate(method, owner, true);
    }
  }
  
  for (auto& methodPair: *instanceMethods) {
    MethodMeta* method = methodPair.second.second;
    BaseClassMeta* owner = methodPair.second.first;
  
    if (method->name == "getTasksWithCompletionHandler:") {
      continue;
    }
    
    if (method->isInit()) {
      if (writeInstanceInits.find(meta->jsName) != writeInstanceInits.end()) {
        writeCreate(method, owner);
      }
    }
    else if (protocolName != "Set" && methodHasGenericParams(method)) {
      if (writeMethodImpls.find(protocolName) != writeMethodImpls.end()) {
        writeMethodImpl(method, owner);
      }
    }
  }

  _buffer << "}\n";
}

void JSExportDefinitionWriter::writeMethodImpl(MethodMeta* method, BaseClassMeta* owner, bool isStatic) {
  bool unavailableInSwift = method->getUnavailableInSwift(owner);
  
  if (unavailableInSwift && !method->isRenamed) {
    _buffer << "  /* ";
  }

  _buffer << method->dumpDeclComments() << endl;
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
  
  if (isStatic) {
    _buffer << "static ";
  }

  string implName = method->jsName;
  
//  if (method->getParamsAsString(owner).find("JSValue") != string::npos) {
//    implName = method->builtName();
//  }
  
//  auto methodImplParams = method->getParamsAsString(owner, ParamCallType::Implementation);
  auto methodImplParams = method->getParamsAsString(owner);

  _buffer << "func " << implName << methodImplParams;

  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  auto& first = *method->signature[0];
  string retTypeString = Type::formatType(first, methodDecl.getReturnType());
  
  if (method->name == "URLFromPasteboard:" ||
      method->name == "URLWithDataRepresentation:relativeToURL:") {
    retTypeString = "NSURL";
  }

  _buffer << " -> " + retTypeString;
  
  string typeNullabilityStr = method->getTypeNullability(owner);
  _buffer << typeNullabilityStr;
  
  _buffer << " {\n";
  _buffer << "    return ";
  
  if (method->getFlags(MetaFlags::MethodHasErrorOutParameter)) {
    _buffer << "try? ";
  }
  
  _buffer << "self.";
  
  string callName = method->jsName;
  
  _buffer << callName;

  auto methodCallParams = method->getParamsAsString(owner, ParamCallType::Call);

  _buffer << methodCallParams << endl;
  
  _buffer << "  }";
  
  if (unavailableInSwift && !method->isRenamed) {
    _buffer << "  */ ";
  }

  _buffer << "\n\n";
}

void JSExportDefinitionWriter::writeCreate(MethodMeta* method, BaseClassMeta* owner, bool isStatic) {
  if (method->constructorTokens.empty() && method->argLabels.empty()) {
    // Empty creates are filtered out earlier, but a few classes
    // have empty inits w/ a different name
    if (method->name == "fieldEditor") {
      cerr << "Skipping empty non-`init` constructor: " << method->name << endl;
      return;
    }
  }
  
  auto methodImplParams = method->getParamsAsString(owner, ParamCallType::Implementation);
  auto methodCallParams = method->getParamsAsString(owner, ParamCallType::Call);
  bool unavailableInSwift = method->getUnavailableInSwift(owner);
  
  if (unavailableInSwift && !method->isRenamed) {
    _buffer << "  /* ";
  }

  _buffer << method->dumpDeclComments() << endl;

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
  
  _buffer << "static func " << method->builtName() << methodImplParams;
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  auto& first = *method->signature[0];
  string retTypeString = Type::formatType(first, methodDecl.getReturnType());
  
  if (method->name == "URLFromPasteboard:" ||
      method->name == "URLWithDataRepresentation:relativeToURL:") {
    retTypeString = "NSURL";
  }

  _buffer << " -> " + retTypeString;
  
  string typeNullabilityStr = method->getTypeNullability(owner);
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

  
  _buffer << "  }";

  if (unavailableInSwift && !method->isRenamed) {
    _buffer << "  */ ";
  }

  _buffer << "\n\n";
}

//void JSExportDefinitionWriter::writeClass(InterfaceMeta* meta, CompoundMemberMap<MethodMeta>* staticMethods, CompoundMemberMap<MethodMeta>* instanceMethods) {
//  string viewName = meta->jsName.substr(2);
//
//  _buffer << "@objc(" << viewName << ") public class " + viewName + ": " + meta->jsName;
//  _buffer << ", " << viewName << "Exports, JSOverridableView {";
//  _buffer << viewOverrides << endl << endl;
//  _buffer << "}\n\n";
//}

void JSExportDefinitionWriter::writeProto(ProtocolMeta* meta) {
  string protoName = meta->jsName;

  bool isProtoClass = overlaidClasses.find(meta->jsName) != overlaidClasses.end();

  if (isProtoClass) {
    protoName = meta->name;
  }

  _buffer << "@objc(" << protoName << ") protocol " << protoName << "Exports: JSExport";
  
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

  output << "var ";
  
  output << ::Meta::sanitizeIdentifierForSwift(name);
  
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
  string returnType = Type::formatType(first, decl->getType(), ignorePointerType);

  //
  // Manual fixes for property return types in the context of a JSExport
  //
  
  // TODO: Better way to detect Any vs AnyObject
  if (returnType == "Any") {
    if (anyObjectProps.find(name) != anyObjectProps.end() && owner->jsName != "NSDraggingItem") {
      returnType = "AnyObject";
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

  // Property return type
  output << ": " << returnType;

  bool isNullable = false;
  
  if (::Meta::nonNullable.find(returnType) == ::Meta::nonNullable.end()) {
    isNullable = true;
  }

  if (returnType == "NSUserInterfaceItemIdentifier") {
    if (owner->jsName == "NSTableColumn" || owner->jsName == "NSLayoutGuide") {
      // Nullable in proto but not impl? How to check for this?
      isNullable = false;
    }
  }
  
  if (isNullable) {
    output << meta->getNullabilitySymbol(owner);
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
      regex re(".*JSValue.*");
      bool returnsJSValue = regex_match(output, re);

      if (returnsJSValue
          && meta->jsName != "URLSessionWebSocketTask"
          && meta->jsName != "NSLayoutAnchor"
          && meta->jsName != "URLSession") {
        _buffer << "// jsvalue ";
      }

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
  
  if (meta->jsName == "AffineTransform") {
    // Unavailable classes
    return;
  }

  CompoundMemberMap<MethodMeta> compoundStaticMethods;
  map<string, bool> addedConstructors = {};

  for (MethodMeta* method : meta->staticMethods) {
    if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
      continue;
    }
    
    if (hiddenNames.find(method->name) != hiddenNames.end()) {
      continue;
    }

    if (method->hasParamOfType("NSInvocation")) {
      // NSInvocation is totally unsupported in Swift
      continue;
    }
    
    if (meta->jsName == "FileHandle") {
      if (method->getFlags(MethodReturnsSelf)) {
        continue;
      }
    }
    
    if (method->isInit()) {
      // Skip empty constructors since bridged NSObject includes this
      if (method->signature.size() <= 1 || (method->argLabels.size() == 2 && method->hasTargetAction())) {
        continue;
      }
      
      string builtName = method->builtName();
      
      if (addedConstructors[builtName]) {
        continue;
      }
      
      addedConstructors[builtName] = true;
    }
    
    compoundStaticMethods.emplace(method->name, make_pair(meta, method));
  }
  
  CompoundMemberMap<MethodMeta> compoundInstanceMethods;
  for (MethodMeta* method : meta->instanceMethods) {
    if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
      continue;
    }
    
    if (hiddenNames.find(method->name) != hiddenNames.end()) {
      continue;
    }
    
    if (addedConstructors[method->jsName]) {
      continue;
    }
    
    if (!method->argLabels.empty()) {
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
    
    // skip instance inits
    if (method->isInit()) {
      if (writeInstanceInits.find(meta->jsName) == writeInstanceInits.end()) {
        //        cout << "Skipping instance init " << method->name << endl;
        continue;
      }
    }
    
    compoundInstanceMethods.emplace(method->name, make_pair(meta, method));
  }
  
  CompoundMemberMap<PropertyMeta> baseClassInstanceProperties;
  CompoundMemberMap<PropertyMeta> ownInstanceProperties;
  
  for (PropertyMeta* property : meta->instanceProperties) {
    if (ownInstanceProperties.find(property->name) != ownInstanceProperties.end()) {
      cerr << "Skipping property with duplicated name: `" << property->name << "`" << endl;
      continue;
    }
    
    auto interface = &meta->as<InterfaceMeta>();

    if (interface->isSubclassOf("NSControl")) {
      if (interface->nameExistsInSuperclass(property->jsName, Method)) {
        cerr << "Skipping property that exists as method in " << interface->jsName << " superclass: `" << property->jsName << "`" << endl;
        continue;
      }
      
      if (property->name == "selectedTag") {
        continue;
      }
      
      if (property->name == "selectedCell") {
        continue;
      }
    }
    
    auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(property->declaration);
    auto& first = *property->getter->signature[0];
    string returnType = Type::formatType(first, decl->getType());
    
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
  CompoundMemberMap<MethodMeta> inheritedInstanceMethods;
  CompoundMemberMap<PropertyMeta> inheritedStaticProperties;
  CompoundMemberMap<PropertyMeta> inheritedInstanceProperties;

  getInheritedMembersRecursive(meta, &inheritedStaticMethods, &inheritedInstanceMethods, &inheritedStaticProperties, &inheritedInstanceProperties);
  
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
//        cerr << "Skipping proto with same name as interface: " << proto.jsName << endl;
        continue;
      }
      
      static unordered_set<string> implementedProtos = {
        "NSUserInterfaceItemIdentification",
        "NSTableViewDelegate",
        "NSObjectProtocol"
      };
      
      if (implementedProtos.find(proto.jsName) == implementedProtos.end()) {
        continue;
      }
      
//      cout << meta->jsName << " proto.jsName: " << proto.jsName << endl;

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
  
  _buffer << meta->dumpDeclComments() << endl << endl;
  _buffer << _docSet.getCommentFor(meta).toString("");
  
  string protocolName = meta->jsName;
  
  bool isProtoClass = overlaidClasses.find(meta->jsName) != overlaidClasses.end();
  
  if (isProtoClass) {
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
//        cerr << "Skipping method `" << methodPair.first << "` in " << protocolName << " because instance property with same name exists" << endl;
        continue;
      }
      
      string output = writeMethod(methodPair, meta, immediateProtocols, "static", metaName);
      
      if (output.size()) {
        _buffer << method->dumpDeclComments() << endl;
        _buffer << _docSet.getCommentFor(method, owner).toString("");

        regex re(".*JSValue.*");
        bool returnsJSValue = regex_match(output, re);
        
        if (returnsJSValue
            && meta->jsName != "NSLayoutAnchor"
            && meta->jsName != "URLSessionWebSocketTask"
            && meta->jsName != "URLSession") {
          _buffer << "// jsvalue ";
        }

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
      
      _buffer << propertyMeta->dumpDeclComments() << endl;
      
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
        cerr << "Skipping method `" << method->jsName << "` because property with same name exists " << endl;
        continue;
      }
      
      string output = writeMethod(methodPair, meta, immediateProtocols, method->isInit() ? "static" : "", metaName);
      
      if (output.size()) {
        _buffer << method->dumpDeclComments() << endl;
        _buffer << "  ";
        _buffer << _docSet.getCommentFor(methodPair.second.second, methodPair.second.first).toString("  ");

        regex re(".*JSValue.*");
        bool returnsJSValue = regex_match(output, re);
        
        if (returnsJSValue
            && meta->jsName != "NSLayoutAnchor"
            && meta->jsName != "URLSessionWebSocketTask"
            && meta->jsName != "URLSession") {
          _buffer << "// jsvalue ";
        }
        
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
        _buffer << propertyMeta->dumpDeclComments() << endl;
        
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
      
      _buffer << propertyMeta->dumpDeclComments() << endl;
      
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
  if (meta->module->Name == "Dispatch") {
    cout << "FunctionMeta: " << meta->module->Name << ": " << meta->name << endl;
  }
  _buffer << meta->dumpDeclComments() << endl;
  _buffer << "// " << meta->name << endl;
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
  if (meta->module->Name == "Dispatch") {
    cout << "VarMeta: " << meta->module->Name << ": " << meta->name << endl;
  }
}

void JSExportDefinitionWriter::visit(MethodMeta* meta)
{
  if (meta->module->Name == "Dispatch") {
    cout << "MethodMeta: " << meta->module->Name << ": " << meta->name << endl;
  }
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
    
    if ((meta->is(MetaType::Interface) || meta->is(MetaType::Protocol))
        && isNotHidden
        && meta->name != "NSResponder"
        && meta->name != "NSColor"
        && meta->name != "NSURLSession"
        && meta->name != "NSURLSessionTask"
        && meta->name != "NSURLSessionWebSocketTask") {
      string filename = meta->jsName + ".swift";
      
      bool isProtoClass = overlaidClasses.find(meta->jsName) != overlaidClasses.end();

      if (isProtoClass) {
        filename = meta->name + ".swift";
      }
      
      if (filename.substr(0, 3) == "URL") {
        filename = "NS" + filename;
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

  jsFile << "import AppKit\n";
  jsFile << "import JavaScriptCore\n";

  /*
   - TODO: Ship an `objc-metadata-generator` binary with the framework,
     so that users can generate files for the frameworks they need.
   
     This is because whether or not we include e.g. CoreSpotlight can effect whether
     or not something in another framework like Foundation gets generated
   
     (e.g. `var contentAttributeSet: CSSearchableItemAttributeSet` in NSUserActivity)
   
     So even if the user doesn't want CoreSpotlight, Foundation will have this field,
     which means ultimately users will want to generate their own bridge files
  */
  
  jsFile << "import Quartz\n";
  jsFile << "import AVKit\n";
  jsFile << "import CoreMedia\n";
  jsFile << "import CoreSpotlight\n";
  jsFile << "import CoreImage\n";
  jsFile << "import CoreGraphics\n";
  jsFile << "import " << frameworkName << "\n";
  
  jsFile << buffer;
  
  jsFile.close();

//  cout << "Wrote " << frameworkName + "/" + filename << endl;
}
}

