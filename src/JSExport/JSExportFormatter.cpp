#include "TypeScript/DefinitionWriter.h"
#include "JSExportDefinitionWriter.h"
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
  { "int", "Int" },
  { "NSConnection", "NSXPCConnection" }
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
    if (ignorePointerType || pointeeTypeName == "NSArray") {
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

bool starts_with(const std::string value, const std::string prefix)
{
  using namespace std;
  
  if (value.length() < prefix.length()) {
    return false;
  }
  
  return std::mismatch(prefix.begin(), prefix.end(), value.begin()).first == prefix.end();
}

bool ends_with(const std::string value, const std::string suffix)
{
  if (suffix.size() > value.size()) return false;
  
  return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
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
    return "UnsafeRawPointer";
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
      regex autoreleasingRe("^\\w+<\\w+,id>$");
      string nullableSuffix = " * _Nullable * _Nullable";
      
      if (regex_match(name, autoreleasingRe)) {
        out += "Autoreleasing";
      }
      else if (ends_with(pointerQualTypeName, nullableSuffix)) {
        if (starts_with(pointerQualTypeName, "NSArray")) {
          out += "Autoreleasing";
        }
        else if (starts_with(pointerQualTypeName, "NSInputStream")) {
          out += "Autoreleasing";
        }
        else if (starts_with(pointerQualTypeName, "NSOutputStream")) {
          out += "Autoreleasing";
        }
      }
      
      unsafeType += "UnsafeMutablePointer";
    }

    out += unsafeType + "<";
  }
  
  // Inside bridged generics, the rules for type names are different,
  // so we don't use nameForJSExport here
  if (pointerType.innerType->is(TypeCString)) {
    out += "UnsafeMutablePointer<UInt8>";
  }
  else if (name == "BOOL") {
    out += "ObjCBool";
  }
  else if (name == "float") {
    out += "Float";
  }
  else if (name == "Array") {
    out += "NSArray";
  }
  else if (name == "String") {
    out += "NSString";
  }
  else if (name.substr(0, 13) == "NSDictionary<") {
    out += "NSDictionary";
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
  string typeStr = "";
  string pointerTypeStr = pointerType.getAsString();
  
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
      if (pointerTypeStr == "NSInteger") {
        typeStr = "Int";
      }
      else {
        typeStr = nameForJSExport(MetaData::renamedName(pointerType.getAsString()));
      }
      break;
    }
    case TypeULong: {
      if (pointerTypeStr == "NSUInteger * _Nonnull") {
        typeStr = "UnsafeMutablePointer<Int>";
      }
      else {
        typeStr = "Int";
      }
      break;
    }
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
      auto pointeeType = pointerType->getPointeeType();
      string pointeeTypeStr = pointeeType.getAsString();
      stripModifiersFromPointerType(pointeeTypeStr);
      regex re("^\\w+<(\\w+)>$");
      string pointeeTypeInnerStr = regex_replace(pointeeTypeStr, re, "$1");

      if (pointeeTypeStr == "Class" || pointeeTypeInnerStr == "Class") {
        return "AnyClass";
      }
      
      typeStr = MetaData::lookupApiNotes(nameForJSExport(pointeeTypeInnerStr)) + ".Type";
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
    case TypeInstancetype:
      typeStr = "Self";
      break;
    case TypeVaList:
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
    
    if (interfaceName == "NSArray" || interfaceName == "NSMutableArray" || interfaceName == "Array") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType, ignorePointerType);
      out += "]";
      return out;
    }
    
    if (interfaceName == "NSDictionary" || interfaceName == "NSMutableDictionary" || interfaceName == "Dictionary") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType, ignorePointerType);
      out += ": ";
      out += formatType(*interfaceType.typeArguments[1], pointerQualType, true);
      out += "]";
      return out;
    }
    
    if (interfaceName == "NSLayoutAnchor") {
      return "JSValue";
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
    auto objectArgs = interfaceObjectType->getTypeArgsAsWritten();

    if (objectArgs.size()) {
      auto argQualType = objectArgs[0];
      string firstArg = argQualType.getAsString(); // `NSString *`
      auto innerTypePtr = argQualType.getTypePtrOrNull();
      if (innerTypePtr->isObjCObjectPointerType()) {
        auto typeArgs = innerTypePtr->getAsObjCInterfacePointerType()->getObjectType()->getTypeArgsAsWritten();
        if (typeArgs.size()) {
          firstArg = typeArgs[0].getAsString();
        }
      }

      stripModifiersFromPointerType(firstArg); // `NSString`
      
      // NSView & NSViewSubclass
      if (interfaceName == "NSView") {
        firstArg = bitwiseView(firstArg);
      }
      
      return nameForJSExport(MetaData::renamedName(firstArg)); // `String`;
    }
    else {
      string pointeeTypeName = typePtr->getPointeeType().getAsString();
      stripModifiersFromPointerType(pointeeTypeName);

      if (pointeeTypeName == "NSArray") {
        return "[Any]";
      }
      else if (pointeeTypeName == "NSDictionary") {
        return "[AnyHashable: Any]";
      }
      else if (interfaceName == "NSObject") {
        return nameForJSExport(MetaData::renamedName(pointeeTypeName));
      }
      else if (interfaceName == "NSView") {
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

string JSExportFormatter::getFunctionProtoCall(string paramName, const vector<Type*>& signature, const clang::QualType qualType) {
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
  
  string blockRetType = formatType(type, qualType, true);
  
  bool isNullableBlockReturn = false;
  
  regex nullableBlockReturn(".* _Nullable.*");
  if (regex_match(qualType.getAsString(), nullableBlockReturn)) {
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
    else {
      output += "return res.to" + blockRetType + "()";
    }
    
    if (isNullableBlockReturn) {
      output += "\n      }\n";
      output += "      return nil";
    }
    else {
//      if (valueTypes.find(blockRetType) == valueTypes.end()) {
//        output += "!\n";
//      }
    }

    cout << blockRetType << " - " << interfaceName << ": " << qualType.getAsString() << endl;
  }

  output += "\n";
  output += "    }";

  return output;
}

string JSExportFormatter::getFunctionProto(const vector<Type*>& signature, const clang::QualType qualType)
{
  return "JSValue";
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

string JSExportFormatter::getMethodParams(MethodMeta* method, BaseClassMeta* owner, ParamCallType callType) {
  string output = "(";
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  const auto parameters = methodDecl.parameters();
  
  if (parameters.empty()) {
    return "()";
  }
  
  size_t lastParamIndex = method->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
  size_t numLabels = method->argLabels.size();

  bool isInit = method->getFlags(MethodIsInitializer) || method->getFlags(MethodReturnsSelf);
  bool isInitWithTargetAction = isInit &&
    numLabels >= 2 &&
    method->argLabels[numLabels - 2] == "target" &&
    method->argLabels[numLabels - 1] == "action";

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
    
    if (method->getFlags(MethodIsInitializer) && method->name != "initByReferencingURL:" && method->name != "initWithCompressionOptions:") {
      if (i < method->constructorTokens.size()) {
        paramName = method->constructorTokens[i];
      }
    }
    else if (idxToLookForName < method->argLabels.size()) {
      paramName = method->argLabels[idxToLookForName];
    }
    
    // Manual fixes for create fns whose renamed params
    // don't seem to be in any attrs or api notes?

    if (method->jsName == "createWithContainerClassDescription") {
      if (method->constructorTokens[i] == "startSpecifier") {
        paramName = "start";
      }
      else if (method->constructorTokens[i] == "endSpecifier") {
        paramName = "end";
      }
    }
    else if (method->jsName == "createWithObjectSpecifier") {
      if (method->constructorTokens[i] == "testObject") {
        paramName = "test";
      }
    }
    else if (method->jsName == "createWithDrawSelector") {
      if (method->constructorTokens[i] == "drawSelector") {
        paramName = "draw";
      }
    }
    else if (method->jsName == "createForURL") {
      if (method->constructorTokens[i] == "forURL") {
        paramName = "for";
      }
      else if (method->constructorTokens[i] == "withContentsOfURL") {
        paramName = "withContentsOf";
      }
    }
    else if (owner->jsName == "URL" && method->isInit()) {
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
    
    auto& type = *method->signature[i+1];
    
    bool ignorePointerType = false;
    
    if (owner->jsName == "Error") {
      // domain wants `String` instead of `NSErrorDomain`
      // userInfo wants `String` instead of `NSError.UserInfoKey`
      if (paramName == "domain" || paramName == "userInfo") {
        ignorePointerType = true;
      }
    }

    string retTypeStr = formatType(type, qualType, ignorePointerType);

    if (callType == Call) {
      // initWithFrame(frame: frame)
      
      if (method->name == "URLWithString:relativeToURL:") {
        cout << "";
      }
      
      if (isInitWithTargetAction && i >= lastParamIndex - 2) {
        output += paramName + ": nil";
      }
      else {
        if (!isGeneratedParamName) {
          if (paramLabel != "_" || method->getFlags(MethodIsInitializer)) {
            // TODO: use more generic logic for multiple same-named params
            if (usedParams[paramName] > 0
                 && method->name == "constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:") {
              output += paramLabel + ": ";
            }
            else if (paramName == "memoryCapacity" && method->name == "initWithMemoryCapacity:diskCapacity:directoryURL:") {
              // umm, ok
              output += "__memoryCapacity: ";
            }
            else if (paramName == "fireDate" && method->name == "initWithFireDate:interval:target:selector:userInfo:repeats:") {
              output += "fireAt: ";
            }
            else if (paramName == "fireDate" && method->name == "initWithFireDate:interval:repeats:block:") {
              output += "fire: ";
            }
            else {
              output += paramName + ": ";
            }
          }
        }
        
        if (retTypeStr == "JSValue") {
          string protoCall = getFunctionProtoCall(paramName, type.as<BlockType>().signature, qualType);
          output += protoCall;
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
        output += paramName;
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
        output += getTypeNullability(&parmVar, method);
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

}
