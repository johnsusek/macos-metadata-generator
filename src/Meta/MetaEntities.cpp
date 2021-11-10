#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include "Meta/Utils.h"
#include "Utils/StringUtils.h"
#include "TypeEntities.h"
#include "MetaEntities.h"
#include "MetaFactory.h"
#include "yaml-cpp/yaml.h"

using namespace std;

Meta::Version Meta::Version::Unknown = UNKNOWN_VERSION;

static void visitBaseClass(Meta::MetaVisitor* visitor, Meta::BaseClassMeta* baseClass)
{
    for (Meta::MethodMeta* method : baseClass->staticMethods) {
        method->visit(visitor);
    }

    for (Meta::MethodMeta* method : baseClass->instanceMethods) {
        method->visit(visitor);
    }

    for (Meta::PropertyMeta* property : baseClass->instanceProperties) {
        property->visit(visitor);
    }

    for (Meta::PropertyMeta* property : baseClass->staticProperties) {
        property->visit(visitor);
    }
}

void Meta::MethodMeta::visit(MetaVisitor* visitor)
{
  visitor->visit(this);
}

void Meta::PropertyMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::EnumConstantMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::CategoryMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
    visitBaseClass(visitor, this);
}

void Meta::InterfaceMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
    visitBaseClass(visitor, this);
}

void Meta::ProtocolMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
    visitBaseClass(visitor, this);
}

void Meta::StructMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::UnionMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::FunctionMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::EnumMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

void Meta::VarMeta::visit(MetaVisitor* visitor)
{
    visitor->visit(this);
}

unordered_set<string> valueTypes = {
  { "Bool" },
  { "String" },
  { "Double" },
  { "Int32" },
  { "UInt32" },
  { "Number" },
  { "Date" },
  { "Array" },
  { "Dictionary" },
  { "Point" },
  { "Range" },
  { "Rect" },
  { "Size" }
};

static unordered_set<string> escapedIdentifiers = {
  "function",
  "arguments",
  "for",
  "defer",
  "self",
  "default"
};

// some types can't be optional when bridged?
unordered_set<string> nonNullable = {
  { "Int" },
  { "CGFloat" },
  { "Bool" },
  { "NSRange" },
  { "NSRect" },
  { "NSTextInputTraitType" },
  { "TimeInterval" }
};

// MARK: - Meta

string Meta::renamedName(string name, string ownerKey) {
  string key = name;

  if (name == "NSDecimal") {
    return name;
  }
  if (name == "NSDecimalNumber") {
    return name;
  }

  if (ownerKey.size()) {
    key = ownerKey + "." + name;
  }

  string newName;
  auto attribute = Type::attributesLookup[key]["renamed"];

  if (attribute != NULL) {
    newName = attribute.as<string>();
  }
  else if (!Type::apiNotes[key].empty()) {
    newName = Type::apiNotes[key];
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

string Meta::sanitizeIdentifierForSwift(const string& identifierName)
{
  if (escapedIdentifiers.find(identifierName) != escapedIdentifiers.end()) {
    return "`" + identifierName + "`";
  }
  else {
    return identifierName;
  }
}

string Meta::jsConversionFnName(string paramName, const Type& type, const clang::QualType qualType) {
  string output;
  string blockRetType = Type::formatType(type, qualType, true);
  bool isNullableBlockReturn = false;
  
  regex nullableBlockReturn(".* _Nullable.*");
  if (regex_match(qualType.getAsString(), nullableBlockReturn)) {
    isNullableBlockReturn = true;
  }
  
  string interfaceName = "";
  
  if (type.is(TypeType::TypeInterface)) {
    const InterfaceMeta& interface = *type.as<InterfaceType>().interface;
    interfaceName = interface.jsName;
  }
  
  string asSymbol = isNullableBlockReturn ? "?" : "!";
  
  if (type.is(TypeType::TypeEnum)) {
    output += blockRetType + ".init(rawValue: Int(res.toInt32()))!";
  }
  else if (interfaceName == "Array") {
    output += paramName + ".toArray()";
    output += " as" + asSymbol + " " + blockRetType;
  }
  else if (interfaceName == "Dictionary") {
    output += paramName + ".toDictionary()";
    output += " as" + asSymbol + " " + blockRetType;
  }
  else if (valueTypes.find(blockRetType) == valueTypes.end()) {
    output += paramName + ".toObjectOf(" + blockRetType + ".self)";
    output += " as" + asSymbol + " " + blockRetType + " ";
  }
  else {
    output += paramName + ".to" + blockRetType + "()";
  }
  
  return output;
}

string Meta::getFunctionInterfaceCall(string paramName, const InterfaceType& type, const clang::QualType qualType) {
  //  @objc func constraint(equalTo: JSValue) -> NSLayoutConstraint {
  //    return self.constraint(equalTo: equalTo.toObjectOf(NSLayoutAnchor.self) as! NSLayoutAnchor<AnchorType>)
  //  }                                 ^ we are building this param value
  const InterfaceMeta& interface = *type.as<InterfaceType>().interface;
  cout << jsConversionFnName(paramName, *type.typeArguments[0], qualType) << endl;
  auto& typeArg = *type.typeArguments[0];
  string blockRetType2 = Type::formatType(typeArg, qualType, true);
  
  string output = paramName + ".toObjectOf(" + interface.jsName + ".self) as! " + interface.jsName + "<" + blockRetType2 + ">";
  
  return output;
}

string Meta::getFunctionProtoCall(string paramName, const vector<Type*>& signature, const clang::QualType qualType) {
  string output;
  
  output += "{ ";
  
  if (signature.size() > 1) {
    for (size_t i = 1; i < signature.size(); i++) {
      output += "p" + std::to_string(i);
      if (i < signature.size() - 1) {
        output += ", ";
      }
    }
    
    output += " in";
  }
  
  output += "\n";
  
  auto& type = *signature[0];
  
  string blockRetType = Type::formatType(type, qualType, true);
  
  bool isNullableBlockReturn = false;
  
  regex nullableBlockReturn(".* _Nullable.*");
  if (blockRetType != "Bool" && regex_match(qualType.getAsString(), nullableBlockReturn)) {
    isNullableBlockReturn = true;
  }
  
  output += "      ";
  
  if (blockRetType != "Void") {
    if (isNullableBlockReturn) {
      output += "if let res = ";
    }
    else {
      output += "let res = ";
    }
  }
  
  output += paramName + ".call(withArguments: [";
  
  for (size_t i = 1; i < signature.size(); i++) {
    output += "p" + std::to_string(i) + " as AnyObject";
    if (i < signature.size() - 1) {
      output += ", ";
    }
  }
  
  output += "])";
  
  if (!isNullableBlockReturn) {
    output += "!";
  }
  
  if (blockRetType != "Void") {
    if (isNullableBlockReturn) {
      output += " { \n";
      output += "        ";
    }
    else {
      output += "\n      ";
    }
    
    string interfaceName = "";
    string asSymbol = isNullableBlockReturn ? "?" : "!";
    
    if (type.is(TypeType::TypeInterface)) {
      const InterfaceMeta& interface = *type.as<InterfaceType>().interface;
      interfaceName = interface.jsName;
    }
    
    if (type.is(TypeType::TypeEnum)) {
      output += "return " + blockRetType + ".init(rawValue: Int(res.toInt32()))!";
    }
    else if (interfaceName == "Array") {
      output += "return res.toArray()";
      output += " as" + asSymbol + " " + blockRetType;
    }
    else if (interfaceName == "Dictionary") {
      output += "return res.toDictionary()";
      output += " as" + asSymbol + " " + blockRetType;
    }
    else if (valueTypes.find(blockRetType) == valueTypes.end()) {
      output += "return res.toObjectOf(" + blockRetType + ".self)";
      output += " as" + asSymbol + " " + blockRetType + " ";
    }
    else if (blockRetType == "Bool") {
      output += "return res.toBool()";
    }
    else {
      output += "return res.to" + blockRetType + "()";
    }

    if (isNullableBlockReturn) {
      output += "\n      }\n";
      output += "      return nil";
    }
  }
  
  output += "\n";
  output += "    }";
  
  return output;
}

// MARK: - MetaMeta

bool Meta::Meta::getUnavailableInSwift(::Meta::Meta* owner) {
  auto ownerKey = owner->jsName;

  if (ownerKey == "NSURL") {
    ownerKey = "URL";
  }

  auto key = ownerKey + "." + this->name;

  // attr lookups use the selector (this->name)
  for (auto attribute: Type::attributesLookup[key]) {
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

  vector<clang::AvailabilityAttr*> availabilityAttributes = ::Meta::Utils::getAttributes<clang::AvailabilityAttr>(*this->declaration);

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

string Meta::Meta::dumpDeclComments(::Meta::Meta* owner) {
  string out;
  
  out += "\n  /**\n";
  out += "    - Selector: " + this->name + "\n";
  
  //  return out;
  //
  //  out += "\n  /**\n";
  //  out += "    - jsName: " + this->jsName + "\n";
  //  out += "    - name: " + this->name + "\n";
  //  out += "    - argLabels: " + StringUtils::join(this->argLabels, ", ") + "\n";
  //
  //  if (this->is(MetaType::Method)) {
  //    MethodMeta& method = this->as<MethodMeta>();
  //    out += "    - constructorTokens: " + StringUtils::join(method.constructorTokens, ", ") + "\n";
  //  }
  //
  //  bool matchedAttrLookup = false;
  //
  //  // Attribute lookups use the selector (this->name)
  //  for (auto attribute: attributesLookup[owner->jsName + "." + this->name]) {
  //    matchedAttrLookup = true;
  //    out += "    - " + attribute.first.as<string>() + ": " + attribute.second.as<string>() + "\n";
  //  }
  //
  //  // Try using swiftName instead of jsName in case the latter didn't work
  //  if (!matchedAttrLookup) {
  //    for (auto attribute: attributesLookup[owner->jsName + "." + this->name]) {
  //      matchedAttrLookup = true;
  //      out += "    - " + attribute.first.as<string>() + ": " + attribute.second.as<string>() + "\n";
  //    }
  //  }
  //
  vector<clang::AvailabilityAttr*> availabilityAttributes = Utils::getAttributes<clang::AvailabilityAttr>(*this->declaration);
  
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

// MARK: - MethodMeta

bool Meta::MethodMeta::hasTargetAction() {
  size_t numArgs = this->argLabels.size();
  if (numArgs >= 2) {
    return this->argLabels[numArgs - 2] == "target" && this->argLabels[numArgs - 1] == "action";
  }
  return false;
}

string Meta::MethodMeta::builtName() {
  std::string output = "";
  std::string selector = this->name;
  std::vector<std::string> selectorTokens;
  std::string prefix = "";

  if (this->name.substr(0, 4) != "init") {
    StringUtils::split(selector, ':', back_inserter(selectorTokens));
  }
  else if (this->constructorTokens.size()) {
    selectorTokens = this->constructorTokens;
  }
  else if (this->argLabels.size()
           && !(this->argLabels.size() == 1 && this->argLabels[0] == "_")) {
    selectorTokens = this->argLabels;
  }
  else if (selector.length()) {
    StringUtils::split(selector, ':', back_inserter(selectorTokens));
  }
    
  if (this->getFlags(MetaFlags::MethodHasErrorOutParameter)) {
    selectorTokens.pop_back();
  }
  
  size_t idx = 0;
  size_t numTokens = this->hasTargetAction() ? selectorTokens.size() - 2 : selectorTokens.size();
  
  for (size_t i = 0; i < numTokens; i++) {
    std::string token = selectorTokens[i];
    if (token == "_") {
      continue;
    }
    idx++;
    token[0] = toupper(token[0]);
    if (idx > 0 && token.substr(0, 4) != "With" && i > 0 && selectorTokens[i-1] != "with") {
      output += "With";
    }
    output += token;
  }
  
  if (this->isInit()) {
    if (output.substr(0, 4) == "with" || output.substr(0, 4) == "With") {
      prefix = "create";
    }
    else {
      prefix = "createWith";
    }
  }
  
  if (prefix.length()) {
    output[0] = toupper(output[0]);
  }
  else {
    output[0] = tolower(output[0]);
  }
   
  output = prefix + output;
  
  output[0] = tolower(output[0]);
  
  std::regex re("^initWith");
  output = std::regex_replace(output, re, "createWith");

  return output;
}

string Meta::MethodMeta::getTypeNullability(BaseClassMeta* owner) {
  string out;
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(this->declaration);
  clang::QualType qualType = methodDecl.getReturnType();
  string returnTypeDecl = qualType.getAsString();
  auto& typeArg = *this->signature[0];
  string retTypeString = Type::formatType(typeArg, qualType);
  clang::Decl::ObjCDeclQualifier qualifiers = methodDecl.getObjCDeclQualifier();
  bool isOptional = false;
  bool isForce = false;
  
  if (returnTypeDecl == "id" && retTypeString == "Any") {
    isForce = true;
  }
  else if (returnTypeDecl == "Class" && retTypeString == "AnyClass") {
    isForce = true;
  }
  
  if (qualifiers & clang::Decl::ObjCDeclQualifier::OBJC_TQ_CSNullability) {
    clang::Optional<clang::NullabilityKind> nullability = clang::AttributedType::stripOuterNullability(qualType);
    isOptional = nullability.getValue() == clang::NullabilityKind::Nullable;
  }
  
  if (isForce) {
    out += "!";
  }
  else if (isOptional) {
    out += "?";
  }
  
  return out;
}

string Meta::MethodMeta::getTypeNullability(clang::ParmVarDecl* decl) {
  ostringstream output;
  auto attrs = decl->getAttrs();
  regex nullableRegex(".* _Nullable$");
  string typeString = decl->getType().getAsString();
  string declName = decl->getNameAsString();
  
  //  cout << declName << " - " << typeString << endl;
  
  if (regex_match(typeString, nullableRegex)) {
    // in rare cases this is needed - like tabGroup in NSWindow - not sure why
    // in theory these should show up in the attrs
    output << "?";
  }
  
  if (typeString == "SEL") {
    output << "!";
  }
  
  return output.str();
}

string Meta::MethodMeta::getParamsAsString(BaseClassMeta* owner, ParamCallType callType) {
  string output = "(";
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(this->declaration);
  const auto parameters = methodDecl.parameters();
  
  if (parameters.empty()) {
    return "()";
  }
  
  size_t lastParamIndex = this->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
  size_t numLabels = this->argLabels.size();  
  bool isInitWithTargetAction = this->isInit() && this->hasTargetAction();
  
  size_t numUnLabeledArgs = lastParamIndex - numLabels;
  size_t numEmptyLabels = 0;
  
  std::map<std::string, size_t> usedParams = {};
  
  for (size_t i = 0; i < lastParamIndex; i++) {
    if (isInitWithTargetAction && callType != Call && i >= lastParamIndex - 2) {
      // remove the target and action params from the init,
      // the cooresponding implementation will then pass nil for these
      continue;
    }
    
    clang::ParmVarDecl parmVar = *parameters[i];
    clang::QualType qualType = parmVar.getType();
    string paramLabel;
    string paramName;
    bool hasUnlabeled = numUnLabeledArgs > numEmptyLabels;
    auto idxToLookForName = i - numEmptyLabels;
    bool isGeneratedParamName = false;
    
    if (hasUnlabeled) {
      paramLabel = "_";
      numEmptyLabels++;
    }
    
    if (this->getFlags(MethodIsInitializer) && this->name != "initByReferencingURL:" && this->name != "initWithCompressionOptions:") {
      if (i < this->constructorTokens.size()) {
        paramName = this->constructorTokens[i];
      }
    }
    else if (idxToLookForName < this->argLabels.size()) {
      paramName = this->argLabels[idxToLookForName];
    }
    
    // Manual fixes for create fns whose renamed params
    // don't seem to be in any attrs or api notes?
    
    if (this->jsName == "createWithContainerClassDescription") {
      if (this->constructorTokens[i] == "startSpecifier") {
        paramName = "start";
      }
      else if (this->constructorTokens[i] == "endSpecifier") {
        paramName = "end";
      }
    }
    else if (this->jsName == "createWithObjectSpecifier") {
      if (this->constructorTokens[i] == "testObject") {
        paramName = "test";
      }
    }
    else if (this->jsName == "createWithDrawSelector") {
      if (this->constructorTokens[i] == "drawSelector") {
        paramName = "draw";
      }
    }
    else if (this->jsName == "createForURL") {
      if (this->constructorTokens[i] == "forURL") {
        paramName = "for";
      }
      else if (this->constructorTokens[i] == "withContentsOfURL") {
        paramName = "withContentsOf";
      }
    }
    else if (owner->jsName == "URL" && this->isInit()) {
      if (paramName == "relativeToURL") {
        paramName = "relativeTo";
      }
      else if (paramName == "fileurlWithPath") {
        paramName = "fileURLWithPath";
      }
      else if (paramName == "fileurlWithFileSystemRepresentation") {
        paramName = "fileURLWithFileSystemRepresentation";
      }
      else if (paramName == "absoluteurlWithDataRepresentation") {
        paramName = "absoluteURLWithDataRepresentation";
      }
    }
    
    if (!hasUnlabeled) {
      paramLabel = paramName;
    }
    
    if (paramName == "_" || paramName.empty()) {
      paramName = "p" + std::to_string(i);
      isGeneratedParamName = true;
    }
    
    if (usedParams[paramName] > 0) {
      paramName += to_string(usedParams[paramName]);
    }
    
    usedParams[paramName]++;
    
    auto& type = *this->signature[i+1];
    
    bool ignorePointerType = false;
    
    if (owner->jsName == "Error") {
      // domain wants `String` instead of `NSErrorDomain`
      // userInfo wants `String` instead of `NSError.UserInfoKey`
      if (paramName == "domain" || paramName == "userInfo") {
        ignorePointerType = true;
      }
    }
    
    string retTypeStr = Type::formatType(type, qualType, ignorePointerType);

    if (callType == Call) {
      // initWithFrame(frame: frame)
      
      if (this->name == "URLWithString:relativeToURL:") {
        cout << "";
      }
      
      if (isInitWithTargetAction && i >= lastParamIndex - 2) {
        output += paramName + ": nil";
      }
      else {
        if (!isGeneratedParamName) {
          if (paramLabel != "_" || this->getFlags(MethodIsInitializer)) {
            // TODO: use more generic logic for multiple same-named params
            if (usedParams[paramName] > 0
                && this->name == "constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:") {
              output += paramLabel + ": ";
            }
            else if (paramName == "memoryCapacity" && this->name == "initWithMemoryCapacity:diskCapacity:directoryURL:") {
              // umm, ok
              output += "__memoryCapacity: ";
            }
            else if (paramName == "fireDate" && this->name == "initWithFireDate:interval:target:selector:userInfo:repeats:") {
              output += "fireAt: ";
            }
            else if (paramName == "fireDate" && this->name == "initWithFireDate:interval:repeats:block:") {
              output += "fire: ";
            }
            else {
              output += paramName + ": ";
            }
          }
        }
        
        if (retTypeStr == "JSValue") {
          if (type.getType() == TypeBlock) {
            string protoCall = getFunctionProtoCall(paramName, type.as<BlockType>().signature, qualType);
            output += protoCall;
          }
          else if (type.getType() == TypeBlock) {
            string protoCall = getFunctionProtoCall(paramName, type.as<FunctionPointerType>().signature, qualType);
            output += protoCall;
          }
          else if (type.getType() == TypeInterface) {
            string protoCall = getFunctionInterfaceCall(paramName, type.as<InterfaceType>(), qualType);
            output += protoCall;
          }
        }
        else {
          output += sanitizeIdentifierForSwift(paramName);
        }
      }
    }
    else if (callType == Implementation) {
      // initWithFrame(frame: CGRect)
      if (paramLabel != paramName) {
        output += paramLabel + " " + paramName;
      }
      else {
        output += "_ " + paramName;
      }
    }
    else if (callType == Definition) {
      // initWithFrame(_: CGRect)
      output += sanitizeIdentifierForSwift(paramLabel);
    }
    
    if (callType != Call) {
      // Param type
      output += ": ";
      
      if (retTypeStr == "[NSFontCollection.MatchingOptionKey: NSNumber]") {
        retTypeStr = "[NSFontCollectionMatchingOptionKey: NSNumber]";
      }
      
      output += retTypeStr;
      
      if (nonNullable.find(retTypeStr) == nonNullable.end()) {
        output += this->getTypeNullability(&parmVar);
      }
    }
    
    if (isInitWithTargetAction && callType != Call) {
      if (i < lastParamIndex - 3) {
        output += ", ";
      }
    }
    else if (i < lastParamIndex - 1) {
      output += ", ";
    }
  }
  
  output += ")";
  
  return output;
}

// MARK: - PropertyMeta

string Meta::PropertyMeta::getNullabilitySymbol(BaseClassMeta* owner) {
  ostringstream output;
  
  auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(this->declaration);
  auto attrs = decl->getPropertyAttributes();
  regex nullableRegex(".* _Nullable$");
  
  if (decl->isOptional()) {
    output << "?";
  }
  else if (attrs & clang::ObjCPropertyDecl::PropertyAttributeKind::OBJC_PR_null_resettable) {
    // weak v strong?
    output << "!";
  }
  else if (attrs & clang::ObjCPropertyDecl::PropertyAttributeKind::OBJC_PR_nullability) {
    output << "?";
  }
  else if (owner->is(MetaType::Protocol) && clang::dyn_cast<clang::ObjCPropertyDecl>(decl)->getPropertyImplementation() == clang::ObjCPropertyDecl::PropertyControl::Optional) {
    output << "?";
  }
  else if (regex_match(decl->getType().getAsString(), nullableRegex)) {
    // in rare cases this is needed - like tabGroup in NSWindow - not sure why
    // in theory these should show up in the attrs
    output << "?";
  }
  
  return output.str();
}

