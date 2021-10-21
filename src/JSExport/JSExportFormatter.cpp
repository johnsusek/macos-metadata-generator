#include "TypeScript/DefinitionWriter.h"
#include "JSExportDefinitionWriter.h"
#include "JSExportMeta.h"
#include "Meta/Utils.h"
#include "Meta/MetaFactory.h"
#include "Meta/MetaData.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>
#include <regex>
#include "yaml-cpp/yaml.h"
#include "JSExportFormatter.h"

namespace TypeScript {
using namespace Meta;
using namespace std;

JSExportFormatter JSExportFormatter::current = JSExportFormatter();

static map<string, string> bridgeNames = {
  { "float", "CGFloat" },
  { "NSString", "String" },
  { "SEL", "String" },
  { "OS_dispatch_queue", "DispatchQueue" },
  { "*", "Any" },
  { "id", "Any" },
  { "void", "Void" },
  { "BOOL", "Bool" },
  { "Class", "AnyClass" },
  { "double", "Double" },
  { "size_t", "Int32st" },
  { "NSUInteger", "Int" },
  { "unsigned int", "UInt" },
  { "uint32_t", "UInt32" },
  { "int32_t", "Int32it" },
  { "uint64_t", "UInt64" },
  { "int64_t", "Int64" },
  { "int", "Int" }
};

string JSExportFormatter::nameForJSExport(const string& jsName)
{
  if (!bridgeNames[jsName].empty()) {
    return bridgeNames[jsName];
  }
  
  return jsName;
}

static unordered_set<string> escapedIdentifiers = {
  "function",
  "arguments",
  "for",
  "defer",
  "self",
  "default"
};

// some types can't be optional when bridged?
unordered_set<string> JSExportFormatter::nonNullable = {
  { "Int" },
  { "CGFloat" },
  { "Bool" },
  { "NSRange" },
  { "NSRect" },
  { "NSTextInputTraitType" },
  { "TimeInterval" }
};

void findAndReplaceIn(string& str, string searchFor, string replaceBy)
{
  size_t found = str.find(searchFor);
  while (found != string::npos) {
    str.replace(found, searchFor.length(), replaceBy);
    found = str.find(searchFor, found + replaceBy.length());
  };
}

static inline void rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), s.end());
}

void JSExportFormatter::stripModifiersFromPointerType(string& name) {
  std::regex re("^NSObject<(\\w+)>$");
  name = std::regex_replace(name, re, "$1");

  findAndReplaceIn(name, "__kindof ", "");
  findAndReplaceIn(name, "const ", "");
  findAndReplaceIn(name, "struct ", "");
  findAndReplaceIn(name, " *", "");
  findAndReplaceIn(name, "<id>", "");
  findAndReplaceIn(name, " _Nullable", "");
  findAndReplaceIn(name, " _Nonnull", "");
  findAndReplaceIn(name, " _Null_unspecified", "");
  rtrim(name);
}

string JSExportFormatter::swiftifyReference(const ::Meta::Meta& meta)
{
  return nameForJSExport(meta.jsName);
}

vector<string> JSExportFormatter::split2(string const &input) {
  istringstream buffer(input);
  
  vector<string> ret{
    istream_iterator<string>(buffer),
    istream_iterator<string>()
  };
  
  return ret;
}

string JSExportFormatter::getTypeNullability(MethodMeta* method, BaseClassMeta* owner) {
  string out;
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  clang::QualType qualType = methodDecl.getReturnType();
  string returnTypeDecl = qualType.getAsString();
  string retTypeString = formatType(*method->signature[0], qualType);
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
 
string JSExportFormatter::getTypeNullability(clang::ParmVarDecl* decl, MethodMeta* meta) {
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

string JSExportFormatter::getNullabilitySymbol(PropertyMeta* meta, BaseClassMeta* owner) {
  ostringstream output;
  
  auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration);
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

string JSExportFormatter::formatTypeId(const IdType& idType, const clang::QualType pointerQualType, const bool ignorePointerType) {
  if (idType.protocols.size() == 1) {
    const ::Meta::Meta& meta = *idType.protocols[0];
    if (meta.jsName != "NSCopying") {
      return meta.jsName;
    }
  }
  
  auto typePtr = pointerQualType.getTypePtrOrNull();

  if (typePtr) {
    string pointeeTypeName = typePtr->getPointeeType().getAsString();
    stripModifiersFromPointerType(pointeeTypeName);
    if (ignorePointerType) {
      return "Any";
    }
    if (pointeeTypeName == "NSArray") {
      return "Any";
    }
    return nameForJSExport(pointeeTypeName);
  }
  else {
    string pointerQualTypeName = pointerQualType.getAsString();
    stripModifiersFromPointerType(pointerQualTypeName);
    return nameForJSExport(pointerQualTypeName);
  }
}

string JSExportFormatter::formatTypePointer(const PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType) {
  string pointerQualTypeName = pointerQualType.getAsString();
  string name = pointerQualTypeName;
  stripModifiersFromPointerType(name);
  name = MetaData::renamedName(name);
  
  bool hasPointerSymbol = pointerQualTypeName.find('*') != std::string::npos;

  if (!hasPointerSymbol) {
    // For e.g. NSRectArray returns that are PointerType but not pointers
    // for our purposes
    return name;
  }
  
  string out;
  string unsafeType = "";

  if (pointerQualTypeName.substr(0, 12) == "const void *") {
    unsafeType = "UnsafeRawPointer";
  }
  else if (pointerType.innerType->is(TypeVoid)) {
    return "UnsafeMutableRawPointer";
  }
  
  bool hasInnerPointer = pointerType.innerType->is(TypePointer);
  
  if (hasInnerPointer) {
    out += "UnsafeMutablePointer<";
  }
  
  bool isInnerNullable = false;
  bool isPointerClassName = false;

  auto typePtr = pointerQualType.getTypePtrOrNull();

  regex pointerClassName(".*Pointer$");
  if (regex_match(name, pointerClassName)) {
    isPointerClassName = true;
  }
  
  if (typePtr && !isPointerClassName) {
    bool isConst = pointerQualTypeName.substr(0, 5) == "const";

    if (isConst) {
      unsafeType += "UnsafePointer";
    }
    else {
      unsafeType += "UnsafeMutablePointer";
    }

    out += unsafeType + "<";
  }
  
  // Inside bridged generics, the rules for type names are different,
  // so we don't use nameForJSExport here
  if (name == "BOOL") {
    out += "ObjCBool";
  }
  else if (name == "float") {
    out += "Float";
  }
  else if (name == "String") {
    out += "NSString";
  }
  else {
    out += nameForJSExport(name);
  }
  
  regex nullableRegex(".* _Nullable$");
  if (regex_match(typePtr->getPointeeType().getAsString(), nullableRegex)) {
    isInnerNullable = true;
  }

  if (hasInnerPointer) {
    out += ">";
  }
  
  if (isInnerNullable) {
    out += "?";
  }
  
  if (typePtr && !isPointerClassName) {
    out += ">";
  }

  return out;
}

string JSExportFormatter::formatType(const Type& type, const clang::QualType pointerType, const bool ignorePointerType)
{
  if (pointerType.getAsString().find("instancetype ", 0) != string::npos) {
    return "Self";
  }
  
  string typeStr = "";
  
  switch (type.getType()) {
    case TypeVoid:
      typeStr = "Void";
      break;
    case TypeBool:
      typeStr = "Bool";
      break;
    case TypeSignedChar:
      typeStr = "Int8";
      break;
    case TypeUnsignedChar:
      typeStr = "UInt8";
      break;
    case TypeShort:
      typeStr = "Int16";
      break;
    case TypeUShort:
//      typeStr = "UInt16";
      typeStr = nameForJSExport(MetaData::renamedName(pointerType.getAsString()));
      break;
    case TypeInt:
      typeStr = "Int32";
      break;
    case TypeUInt:
//      typeStr = "UInt";
      typeStr = nameForJSExport(MetaData::renamedName(pointerType.getAsString()));
      break;
    case TypeLong: {
      typeStr = nameForJSExport(MetaData::renamedName(pointerType.getAsString()));
//      typeStr = "Int";
      break;
    }
    case TypeULong:
//      typeStr = nameForJSExport(MetaData::renamedName(pointerType.getAsString()));
      typeStr = "Int";
      break;
    case TypeLongLong:
      typeStr = "Int64";
      break;
    case TypeULongLong:
      typeStr = "UInt64";
      break;
    case TypeFloat:
    case TypeDouble: {
      if (pointerType.getAsString() == "float") {
        return "Float";
      }
      else if (ignorePointerType || pointerType.getAsString() == "CGFloat") {
        typeStr = "CGFloat";
      }
      else {
        typeStr = nameForJSExport(pointerType.getAsString());
      }
      break;
    }
    case TypeSelector:
      typeStr = "Selector";
      break;
    case TypeUnichar:
      typeStr = "String /* TypeUnichar */ ";
      break;
    case TypeCString: {
      string pointerTypeStr = pointerType.getAsString();
      
      bool isMutable = pointerTypeStr.substr(0, 5) != "const";
      bool isUnsigned = false;
      
      if (isMutable) {
        isUnsigned = pointerTypeStr.substr(0, 8) == "unsigned";
        typeStr = "UnsafeMutablePointer<";
      }
      else {
        isUnsigned = pointerTypeStr.substr(6, 8) == "unsigned";
        typeStr = "UnsafePointer<";
      }
      
      if (isUnsigned) {
        typeStr += "UInt8";
      }
      else {
        typeStr += "Int8";
      }
      
      typeStr += ">";

      break;
    }
    case TypeProtocol:
      typeStr = "Protocol";
      break;
    case TypeClass: {
      auto it = type.as<PointerType>().innerType;
      auto pointeeType = pointerType->getPointeeType();
      
      if (it != NULL && pointeeType.getAsString() != "Class") {
        string str = pointeeType.getAsString();
        str.resize(str.size() - 1);
        str.replace(0, 6, "");
        typeStr = MetaData::lookupApiNotes(nameForJSExport(str)) + ".Type";
      }
      else if (pointeeType.getAsString() == "Class") {
        return "AnyClass";
      }
      else {
        typeStr = "NSObject";
      }
      break;
    }
    case TypeStruct:
      typeStr = swiftifyReference(*type.as<StructType>().structMeta);
      break;
    case TypeUnion:
      typeStr = swiftifyReference(*type.as<UnionType>().unionMeta);
      break;
    case TypeEnum:
      typeStr = swiftifyReference(*type.as<EnumType>().enumMeta);
      break;
    case TypeTypeArgument:
      typeStr = type.as<TypeArgumentType>().name;
      break;
    case TypeBlock:
      typeStr = getFunctionProto(type.as<BlockType>().signature, pointerType);
      break;
    case TypeFunctionPointer:
      typeStr = getFunctionProto(type.as<FunctionPointerType>().signature, pointerType);
      break;
    case TypeConstantArray:
    case TypeExtVector: {
      string name = MetaData::lookupApiNotes(formatType(*type.as<ConstantArrayType>().innerType, pointerType));
      typeStr = "UnsafeMutablePointer<" + name + ">";
      break;
    }
    case TypeIncompleteArray:
      typeStr = formatType(*type.as<IncompleteArrayType>().innerType, pointerType);
      break;
    case TypeId:
      typeStr = formatTypeId(type.as<IdType>(), pointerType, ignorePointerType);
      break;
    case TypePointer:
      typeStr = formatTypePointer(type.as<PointerType>(), pointerType, ignorePointerType);
      break;
    case TypeInterface:
    case TypeBridgedInterface:
      typeStr = formatTypeInterface(type, pointerType, ignorePointerType);
      break;
    case TypeAnonymousStruct:
    case TypeAnonymousUnion:
      typeStr = formatTypeAnonymous(type, pointerType);
      break;
    case TypeVaList:
    case TypeInstancetype:
    default:
      break;
  }
  
  return MetaData::lookupApiNotes(typeStr);
}

string JSExportFormatter::formatTypeAnonymous(const Type& type, const clang::QualType pointerType) {
  string output;
  
  output += "{ ";
  
  const vector<RecordField>& fields = type.as<AnonymousStructType>().fields;
  
  for (auto& field : fields) {
    output += field.name + ": " + nameForJSExport(formatType(*field.encoding, pointerType)) + "; ";
  }
  
  output += "}";
  
  return output;
}

// NSView<NSCollectionViewElement>
// to
// (NSView & NSCollectionViewElement)
string JSExportFormatter::bitwiseView(string& name) {
  if (name.substr(0, 7) == "NSView<") {
    name.pop_back();
    name = name.substr(7, name.size());
    auto argName = nameForJSExport(MetaData::renamedName(name));
    return "(NSView & " + argName + ")";
  }
  
  return name;
}

string JSExportFormatter::formatTypeInterface(const Type& type, const clang::QualType pointerQualType, const bool ignorePointerType) {
  if (type.is(TypeType::TypeBridgedInterface) && type.as<BridgedInterfaceType>().isId()) {
    return formatType(IdType(), pointerQualType);
  }
  
  string pointerQualTypeStr = pointerQualType.getAsString();
  const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
  string interfaceName = nameForJSExport(MetaData::renamedName(interface.jsName));

  if (DefinitionWriter::hasClosedGenerics(type)) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    
    if (interfaceName == "NSArray" || interfaceName == "Array") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType);
      out += "]";
      return out;
    }
    
    if (interfaceName == "NSDictionary" || interfaceName == "Dictionary") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType, ignorePointerType);
      out += ": ";
      out += formatType(*interfaceType.typeArguments[1], pointerQualType, true);
      out += "]";
      return out;
    }
    
    string typeArgName;
    
    if (interfaceType.typeArguments.empty()) {
      // for NSCandidateListTouchBarItem which is generic,
      // but doesn't have type args?
      typeArgName = "AnyObject";
    }
    else {
      typeArgName = formatType(*interfaceType.typeArguments[0], pointerQualType);
      stripModifiersFromPointerType(typeArgName);
    }
    
    string out = interfaceName + "<" + typeArgName + ">";
    
    return out;
  }
  
  if (ignorePointerType) {
    return interfaceName;
  }
  
  // No generics, so we just want the inner arg type
  // e.g. NSArray<NSAppearanceName> -> NSAppearanceName
  
  auto typePtr = pointerQualType.getTypePtrOrNull();
  
  if (typePtr->isObjCObjectPointerType()) {
    // At this point, pointerQualTypeStr looks like this:
    // NSArray<NSString *> * _Nonnull
    //
    // and pointeeTypeStr looks like this:
    // NSArray<NSString *>
    //
    // We will get the inner `NSString` here
    //
    auto typePtrInterfaceType = typePtr->getAsObjCInterfacePointerType();
    auto interfaceObjectType = typePtrInterfaceType->getObjectType();
    auto ojectArgs = interfaceObjectType->getTypeArgsAsWritten();

    if (ojectArgs.size()) {
      string firstArg = ojectArgs[0].getAsString(); // `NSString *`
      stripModifiersFromPointerType(firstArg); // `NSString`
      if (interfaceName == "NSView") {
        firstArg = bitwiseView(firstArg);
      }
      return nameForJSExport(MetaData::renamedName(firstArg)); // `String`;
    }
    else {
      string pointeeTypeName = typePtr->getPointeeType().getAsString();
      if (interfaceName == "NSObject") {
        stripModifiersFromPointerType(pointeeTypeName);
        return nameForJSExport(MetaData::renamedName(pointeeTypeName));
      }
      else if (interfaceName == "NSView") {
        stripModifiersFromPointerType(pointeeTypeName);
        pointeeTypeName = bitwiseView(pointeeTypeName);
        return nameForJSExport(MetaData::renamedName(pointeeTypeName));
      }
      else {
        stripModifiersFromPointerType(pointerQualTypeStr);
        return nameForJSExport(MetaData::renamedName(pointerQualTypeStr));
      }
    }
  }
  else {
    stripModifiersFromPointerType(pointerQualTypeStr);
    return nameForJSExport(MetaData::renamedName(pointerQualTypeStr));
  }
  
  throw logic_error(string("Misparsed interface definition '") + interfaceName + "'.");
}

string JSExportFormatter::getFunctionProto(const vector<Type*>& signature, const clang::QualType qualType)
{
  // Change:
  // @objc @available(OSX 10.6, *) func enumerateObjectsUsingBlock(block: (p1: ObjectType, p2: Bool) => Void) -> Void
  
  // To:
  // @available(OSX 10.6, *) @objc func enumerateObjects(_Cb block: JSValue)
  
  // Then add
  
  // extension NSSet: NSSetJSExport {
  //   func enumerateObjects(_Cb block: JSValue) -> Void {
  //     return self.enumerateObjects(BridgeHelpers.VoidCallbackEnumerator(block))
  //   }
  // }
  
  // TODO: Decide which BridgeHelpers.xxxCallbackEnumerator to use
  ostringstream output;
  output << "JSValue";
  
  return output.str();
}

string JSExportFormatter::sanitizeIdentifierForSwift(const string& identifierName)
{
  if (escapedIdentifiers.find(identifierName) != escapedIdentifiers.end()) {
    return "`" + identifierName + "`";
  }
  else {
    return identifierName;
  }
}

string JSExportFormatter::getMethodParams(MethodMeta* method, BaseClassMeta* owner, bool forCall) {
  string output = "(";
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  const auto parameters = methodDecl.parameters();
  size_t lastParamIndex = method->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
  size_t numEmptyLabels = 0;
  size_t numLabels = method->argLabels.size();
  size_t numUnLabeledArgs = lastParamIndex - numLabels;

  for (size_t i = 0; i < lastParamIndex; i++) {
    clang::ParmVarDecl parmVar = *parameters[i];
    clang::QualType qualType = parmVar.getType();
    string paramLabel;
    string paramName;
    bool hasUnlabeled = numUnLabeledArgs > numEmptyLabels;
    auto idxToLookForName = i - numEmptyLabels;

    if (hasUnlabeled) {
        paramLabel = "_";
        numEmptyLabels++;
        if (numLabels > 0) {
          if (i < numLabels) {
            paramName = method->argLabels[i];
          }
          else if (i < parameters.size()) {
            cout << "Warning: fell back to param label instead of argument label for " << method->name << endl;
            paramName = parameters[i]->getNameAsString();
          }
        }
      }
      else {
        // We have to use objc selector labels for param names in bridge, to avoid
        // "method has different argument labels from those required by protocol" errors
        paramName = method->argLabels[idxToLookForName];
        paramLabel = paramName;
      }
    
    auto paramLabelParts = split2(paramLabel);
    auto paramLabelFirst = paramLabelParts[0];
    
    output += sanitizeIdentifierForSwift(paramLabelFirst);
    
    // Param return type
    output += ": ";
    
    auto& type = *method->signature[i+1];

    if (DefinitionWriter::hasClosedGenerics(type)) {
      output += "JSValue";
    }
    else {
      string retTypeStr = formatType(type, qualType);
      output += retTypeStr;
      if (nonNullable.find(retTypeStr) == nonNullable.end()) {
        output += getTypeNullability(&parmVar, method);
      }
    }
    
    if (i < lastParamIndex - 1) {
      output += ", ";
    }
  }
  
  output += ")";
  
  return output;
}

}
