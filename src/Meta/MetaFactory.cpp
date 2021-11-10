#include "MetaFactory.h"
#include "MetaEntities.h"
#include "CreationException.h"
#include "JSExport/JSExportDefinitionWriter.h"
#include "Utils.h"
#include "Utils/StringUtils.h"
#include "ValidateMetaTypeVisitor.h"
#include <iostream>
#include <string>
#include <sstream>
#include "Utils/pstream.h"
#include "yaml-cpp/yaml.h"

using namespace std;

namespace Meta {

static bool compareJsNames(string& protocol1, string& protocol2)
{
  string name1 = protocol1;
  string name2 = protocol2;
  transform(name1.begin(), name1.end(), name1.begin(), ::tolower);
  transform(name2.begin(), name2.end(), name2.begin(), ::tolower);
  return name1 < name2;
}

static bool metasComparerByJsName(Meta* meta1, Meta* meta2)
{
  return compareJsNames(meta1->jsName, meta2->jsName);
}

void MetaFactory::validate(Type* type)
{
  ValidateMetaTypeVisitor validator(*this);
  
  type->visit(validator);
}

void MetaFactory::validate(Meta* meta)
{
  auto declIt = this->_metaToDecl.find(meta);
  if (declIt == this->_metaToDecl.end()) {
    throw MetaCreationException(meta, "Metadata not created", true);
  }
  
  auto metaIt = this->_cache.find(declIt->second);
  assert(metaIt != this->_cache.end());
  if (metaIt->second.second.get() != nullptr) {
    //        printf("**** Validation failed for %s: %s ***\n\n", meta->name.c_str(), metaIt->second.second.c_str());
    POLYMORPHIC_THROW(metaIt->second.second);
  }
}

string MetaFactory::getTypedefOrOwnName(const clang::TagDecl* tagDecl)
{
  assert(tagDecl);
  
  if (tagDecl->getNextDeclInContext() != nullptr) {
    if (const clang::TypedefDecl* nextDecl = clang::dyn_cast<clang::TypedefDecl>(tagDecl->getNextDeclInContext())) {
      
      if (const clang::ElaboratedType* innerElaboratedType = clang::dyn_cast<clang::ElaboratedType>(nextDecl->getUnderlyingType().getTypePtr())) {
        if (const clang::TagType* tagType = clang::dyn_cast<clang::TagType>(innerElaboratedType->desugar().getTypePtr())) {
          if (tagType->getDecl() == tagDecl) {
            return nextDecl->getFirstDecl()->getNameAsString();
          }
        }
      }
    }
  }
  
  // The decl has no typedef name, so we return its name.
  return tagDecl->getNameAsString();
}

template<class T>
void resetMetaAndAddToMap(unique_ptr<Meta>& metaPtrRef, MetaToDeclMap& metaToDecl, const clang::Decl& decl) {
  if (metaPtrRef.get()) {
    // The pointer has been previously allocated. Reset it's value and assert that it's already present in the map
    static_cast<T&>(*metaPtrRef) = T();
    assert(metaToDecl[metaPtrRef.get()] == &decl);
  } else {
    // Allocate memory and add to map
    metaPtrRef.reset(new T());
    metaToDecl[metaPtrRef.get()] = &decl;
  }
  
  if (decl.isInvalidDecl()) {
    string declDump;
    llvm::raw_string_ostream os(declDump);
    decl.dump(os);
    throw MetaCreationException(metaPtrRef.get(), CreationException::constructMessage("", os.str()), true);
  }
}

Meta* MetaFactory::create(const clang::Decl& decl, bool resetCached /* = false*/, string categoryName)
{
  // Check for cached Meta
  Cache::iterator cachedMetaIt = _cache.find(&decl);
  if (!resetCached && cachedMetaIt != _cache.end()) {
    Meta* meta = cachedMetaIt->second.first.get();
    if (auto creationException = cachedMetaIt->second.second.get()) {
      POLYMORPHIC_THROW(creationException);
    }
    
    /* TODO: The meta object is not guaranteed to be fully initialized. If the meta object is in the creation stack
     * it will appear in cache, but will not be fully initialized. This may cause some inconsistent results.
     * */
    
    return meta;
  }
  
  if (cachedMetaIt == _cache.end()) {
    pair<Cache::iterator, bool> insertionResult = _cache.insert(make_pair(&decl, make_pair(nullptr, nullptr)));
    assert(insertionResult.second);
    cachedMetaIt = insertionResult.first;
  }
  unique_ptr<Meta>& insertedMetaPtrRef = cachedMetaIt->second.first;
  unique_ptr<CreationException>& insertedException = cachedMetaIt->second.second;
  
  try {
    if (const clang::FunctionDecl* function = clang::dyn_cast<clang::FunctionDecl>(&decl)) {
      resetMetaAndAddToMap<FunctionMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*function, *insertedMetaPtrRef.get(), categoryName);
      createFromFunction(*function, insertedMetaPtrRef.get()->as<FunctionMeta>());
    }
    else if (const clang::RecordDecl* record = clang::dyn_cast<clang::RecordDecl>(&decl)) {
      if (record->isStruct()) {
        resetMetaAndAddToMap<StructMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
        populateIdentificationFields(*record, *insertedMetaPtrRef.get(), categoryName);
        createFromStruct(*record, insertedMetaPtrRef.get()->as<StructMeta>());
      } else {
        resetMetaAndAddToMap<UnionMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
        populateIdentificationFields(*record, *insertedMetaPtrRef.get(), categoryName);
        throw MetaCreationException(insertedMetaPtrRef.get(), "The record is union.", false);
      }
    }
    else if (const clang::VarDecl* var = clang::dyn_cast<clang::VarDecl>(&decl)) {
      resetMetaAndAddToMap<VarMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*var, *insertedMetaPtrRef.get(), categoryName);
      createFromVar(*var, insertedMetaPtrRef.get()->as<VarMeta>());
    }
    else if (const clang::EnumDecl* enumDecl = clang::dyn_cast<clang::EnumDecl>(&decl)) {
      resetMetaAndAddToMap<EnumMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*enumDecl, *insertedMetaPtrRef.get(), categoryName);
      createFromEnum(*enumDecl, insertedMetaPtrRef.get()->as<EnumMeta>());
    }
    else if (const clang::EnumConstantDecl* enumConstantDecl = clang::dyn_cast<clang::EnumConstantDecl>(&decl)) {
      resetMetaAndAddToMap<EnumConstantMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*enumConstantDecl, *insertedMetaPtrRef.get(), categoryName);
      createFromEnumConstant(*enumConstantDecl, insertedMetaPtrRef.get()->as<EnumConstantMeta>());
    }
    else if (const clang::ObjCInterfaceDecl* interface = clang::dyn_cast<clang::ObjCInterfaceDecl>(&decl)) {
      resetMetaAndAddToMap<InterfaceMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*interface, *insertedMetaPtrRef.get(), categoryName);
      createFromInterface(*interface, insertedMetaPtrRef.get()->as<InterfaceMeta>());
    }
    else if (const clang::ObjCProtocolDecl* protocol = clang::dyn_cast<clang::ObjCProtocolDecl>(&decl)) {
      resetMetaAndAddToMap<ProtocolMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*protocol, *insertedMetaPtrRef.get(), categoryName);
      createFromProtocol(*protocol, insertedMetaPtrRef.get()->as<ProtocolMeta>());
    }
    else if (const clang::ObjCCategoryDecl* category = clang::dyn_cast<clang::ObjCCategoryDecl>(&decl)) {
      resetMetaAndAddToMap<CategoryMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*category, *insertedMetaPtrRef.get(), categoryName);
      createFromCategory(*category, insertedMetaPtrRef.get()->as<CategoryMeta>());
    }
    else if (const clang::ObjCMethodDecl* method = clang::dyn_cast<clang::ObjCMethodDecl>(&decl)) {
      resetMetaAndAddToMap<MethodMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*method, *insertedMetaPtrRef.get(), categoryName);
      createFromMethod(*method, insertedMetaPtrRef.get()->as<MethodMeta>());
    }
    else if (const clang::ObjCPropertyDecl* property = clang::dyn_cast<clang::ObjCPropertyDecl>(&decl)) {
      resetMetaAndAddToMap<PropertyMeta>(insertedMetaPtrRef, this->_metaToDecl, decl);
      populateIdentificationFields(*property, *insertedMetaPtrRef.get(), categoryName);
      createFromProperty(*property, insertedMetaPtrRef.get()->as<PropertyMeta>());
    } else {
      throw logic_error("Unknown declaration type.");
    }
    
    return insertedMetaPtrRef.get();
  } catch (MetaCreationException& e) {
    if (e.getMessage().size()) {
      cerr << e.getDetailedMessage() << endl;
    }
    
    if (e.getMeta() == insertedMetaPtrRef.get()) {
      insertedException = llvm::make_unique<MetaCreationException>(e);
      throw;
    }
    string message = CreationException::constructMessage("Can't create meta dependency.", e.getDetailedMessage());
    insertedException = llvm::make_unique<MetaCreationException>(insertedMetaPtrRef.get(), message, e.isError());
    POLYMORPHIC_THROW(insertedException);
  } catch (TypeCreationException& e) {
    string message = CreationException::constructMessage("Can't create type dependency.", e.getDetailedMessage());
    insertedException = llvm::make_unique<MetaCreationException>(insertedMetaPtrRef.get(), message, e.isError());
    cerr << e.getDetailedMessage() << endl;
    POLYMORPHIC_THROW(insertedException);
  }
}

bool MetaFactory::tryCreate(const clang::Decl& decl, Meta** meta, string categoryName)
{
  try {
    Meta* result = this->create(decl, false, categoryName);
    if (meta != nullptr) {
      *meta = result;
    }
    return true;
  } catch (CreationException& e) {
    if (e.getMessage().size()) {
      cerr << e.getMessage() << endl;
    }
    return false;
  }
}

void MetaFactory::createFromFunction(const clang::FunctionDecl& function, FunctionMeta& functionMeta)
{
  if (function.isThisDeclarationADefinition()) {
    throw MetaCreationException(&functionMeta, "", false);
  }
  
  // TODO: We don't support variadic functions but we save in metadata flags whether a function is variadic or not.
  // If we not plan in the future to support variadic functions this redundant flag should be removed.
  if (function.isVariadic())
    throw MetaCreationException(&functionMeta, "", false);
  
  populateMetaFields(function, functionMeta);
  
  functionMeta.setFlags(MetaFlags::FunctionIsVariadic, function.isVariadic()); // set IsVariadic
  
  // set signature
  functionMeta.signature.push_back(_typeFactory.create(function.getReturnType()).get());
  for (clang::ParmVarDecl* param : function.parameters()) {
    functionMeta.signature.push_back(_typeFactory.create(param->getType()).get());
  }
  
  bool returnsRetained = function.hasAttr<clang::NSReturnsRetainedAttr>() || function.hasAttr<clang::CFReturnsRetainedAttr>();
  bool returnsNotRetained = function.hasAttr<clang::NSReturnsNotRetainedAttr>() || function.hasAttr<clang::CFReturnsNotRetainedAttr>();
  
  // Clang doesn't handle The Create Rule automatically like for methods, so we have to do it manually
  if (!(returnsRetained || returnsNotRetained) && functionMeta.signature[0]->is(TypeBridgedInterface)) {
    if (function.hasAttr<clang::CFAuditedTransferAttr>()) {
      string functionName = function.getNameAsString();
      if (functionName.find("Create") != string::npos || functionName.find("Copy") != string::npos) {
        returnsRetained = true;
      }
    } else {
      functionMeta.setFlags(MetaFlags::FunctionReturnsUnmanaged, true);
    }
  }
  
  functionMeta.setFlags(MetaFlags::FunctionOwnsReturnedCocoaObject, returnsRetained); // set OwnsReturnedCocoaObjects
}

void MetaFactory::createFromStruct(const clang::RecordDecl& record, StructMeta& structMeta)
{
  if (!record.isStruct())
    throw MetaCreationException(&structMeta, "", false);
  if (!record.isThisDeclarationADefinition()) {
    throw MetaCreationException(&structMeta, "", false);
  }
  
  populateMetaFields(record, structMeta);

  // set fields
  for (clang::FieldDecl* field : record.fields()) {
    RecordField recordField(field->getNameAsString(), _typeFactory.create(field->getType()).get());
    structMeta.fields.push_back(recordField);
  }
}

void MetaFactory::createFromVar(const clang::VarDecl& var, VarMeta& varMeta)
{
  if (var.getLexicalDeclContext() != var.getASTContext().getTranslationUnitDecl()) {
    throw MetaCreationException(&varMeta, "", false);
  }
  
  populateMetaFields(var, varMeta);
  //set type
  varMeta.signature = _typeFactory.create(var.getType()).get();
  varMeta.hasValue = false;
  
  if (var.hasInit()) {
    clang::APValue* evValue = var.evaluateValue();
    if (evValue == nullptr) {
      throw MetaCreationException(&varMeta, "Unable to evaluate compile-time constant value.", false);
    }
    
    varMeta.hasValue = true;
    llvm::SmallVector<char, 10> valueAsString;
    
    switch (evValue->getKind()) {
      case clang::APValue::ValueKind::Int:
        evValue->getInt().toString(valueAsString, 10, evValue->getInt().isSigned());
        break;
      case clang::APValue::ValueKind::Float:
        evValue->getFloat().toString(valueAsString);
        break;
      case clang::APValue::ValueKind::ComplexInt:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::ComplexFloat:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::AddrLabelDiff:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::Array:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::LValue:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::MemberPointer:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::Struct:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::Union:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::Vector:
        throw MetaCreationException(&varMeta, "", false);
      case clang::APValue::ValueKind::Uninitialized:
        throw MetaCreationException(&varMeta, "", false);
      default:
        throw MetaCreationException(&varMeta, "", false);
    }
    
    varMeta.value = string(valueAsString.data(), valueAsString.size());
  }
}

void MetaFactory::createFromEnum(const clang::EnumDecl& enumeration, EnumMeta& enumMeta)
{
  if (!enumeration.isThisDeclarationADefinition()) {
    throw MetaCreationException(&enumMeta, "Forward declaration of enum.", false);
  }
  
  populateMetaFields(enumeration, enumMeta);
  
  vector<string> fieldNames;
  for (clang::EnumConstantDecl* enumField : enumeration.enumerators())
    fieldNames.push_back(enumField->getNameAsString());
  size_t fieldNamePrefixLength = Utils::calculateEnumFieldsPrefix(enumMeta.jsName, fieldNames).size();
  
  for (clang::EnumConstantDecl* enumField : enumeration.enumerators()) {
    // Convert values having the signed bit set to 1 to signed in order to represent them correctly in JS (-1, -2, etc)
    // NOTE: Values having bits 53 to 62 different than the sign bit will continue to not be represented exactly
    // as MAX_SAFE_INTEGER is 2 ^ 53 - 1
    bool asSigned = enumField->getInitVal().isSigned() || enumField->getInitVal().getActiveBits() > 63;
    string valueStr = enumField->getInitVal().toString(10, asSigned);
    
    if (fieldNamePrefixLength > 0) {
      enumMeta.swiftNameFields.push_back({ enumField->getNameAsString().substr(fieldNamePrefixLength, string::npos), valueStr });
    }
    enumMeta.fullNameFields.push_back({ enumField->getNameAsString(), valueStr });
  }
  
  // Lowercase enum values to match swift
  //
  // BackingStoreType.Retained -> BackingStoreType.retained
  // StyleMask.HUDWindow -> StyleMask.HUDWindow
  //
//  for (auto nameField : enumMeta.swiftNameFields) {
//    if (nameField.name.size() > 1 && islower(nameField.name[1])) {
//      nameField.name[0] = tolower(nameField.name[0]);
//    }
//  }
}

void MetaFactory::createFromEnumConstant(const clang::EnumConstantDecl& enumConstant, EnumConstantMeta& enumConstantMeta)
{
  populateMetaFields(enumConstant, enumConstantMeta);
  
  llvm::SmallVector<char, 10> value;
  enumConstant.getInitVal().toString(value, 10, enumConstant.getInitVal().isSigned());
  enumConstantMeta.value = string(value.data(), value.size());
  
  const clang::EnumDecl* parent = clang::cast<clang::EnumDecl>(enumConstant.getDeclContext());
  EnumMeta& parentMeta = this->_cache.find(parent)->second.first.get()->as<EnumMeta>();
  enumConstantMeta.isScoped = !parentMeta.jsName.empty();
}

void MetaFactory::createFromInterface(const clang::ObjCInterfaceDecl& interface, InterfaceMeta& interfaceMeta)
{
  if (!interface.isThisDeclarationADefinition()) {
    throw MetaCreationException(&interfaceMeta, "", false);
  }
  
  populateMetaFields(interface, interfaceMeta);
  populateBaseClassMetaFields(interface, interfaceMeta);
  
  // set base interface
  clang::ObjCInterfaceDecl* super = interface.getSuperClass();
  interfaceMeta.base = (super == nullptr || super->getDefinition() == nullptr) ? nullptr : &this->create(*super->getDefinition())->as<InterfaceMeta>();
}

void MetaFactory::createFromProtocol(const clang::ObjCProtocolDecl& protocol, ProtocolMeta& protocolMeta)
{
  if (!protocol.isThisDeclarationADefinition()) {
    throw MetaCreationException(&protocolMeta, "", false);
  }
  
  populateMetaFields(protocol, protocolMeta);
  populateBaseClassMetaFields(protocol, protocolMeta);
}

void MetaFactory::createFromCategory(const clang::ObjCCategoryDecl& category, CategoryMeta& categoryMeta)
{
  populateMetaFields(category, categoryMeta);
  populateBaseClassMetaFields(category, categoryMeta);
  categoryMeta.extendedInterface = &this->create(*category.getClassInterface()->getDefinition())->as<InterfaceMeta>();
}

void MetaFactory::createFromMethod(const clang::ObjCMethodDecl& method, MethodMeta& methodMeta)
{
  populateMetaFields(method, methodMeta);
  
  methodMeta.setFlags(MetaFlags::MemberIsOptional, method.isOptional());
  methodMeta.setFlags(MetaFlags::MethodIsVariadic, method.isVariadic()); // set IsVariadic flag
  
  bool isNullTerminatedVariadic = method.isVariadic() && method.hasAttr<clang::SentinelAttr>(); // set MethodIsNilTerminatedVariadic flag
  methodMeta.setFlags(MetaFlags::MethodIsNullTerminatedVariadic, isNullTerminatedVariadic);
  
  // set MethodHasErrorOutParameter flag
  if (method.parameters().size() > 0) {
    clang::ParmVarDecl* lastParameter = method.parameters()[method.parameters().size() - 1];
    Type* type = _typeFactory.create(lastParameter->getType()).get();
    if (type->is(TypeType::TypePointer)) {
      Type* innerType = type->as<PointerType>().innerType;
      if (innerType->is(TypeType::TypeInterface) && innerType->as<InterfaceType>().interface->jsName == "Error") {
        methodMeta.setFlags(MetaFlags::MethodHasErrorOutParameter, true);
      }
    }
  }
  
  bool isInitializer = method.getMethodFamily() == clang::ObjCMethodFamily::OMF_init;
  methodMeta.setFlags(MetaFlags::MethodIsInitializer, isInitializer); // set MethodIsInitializer flag
  if (isInitializer) {
    assert(methodMeta.getSelector().find("init", 0) == 0);
    
    // For JSExport bridging, we s/init/create since it does not support 'init' keyword
    regex re("^init");
    methodMeta.jsName = regex_replace(methodMeta.jsName, re, "create");

    string initPrefix = methodMeta.getSelector().find("initWith", 0) == 0 ? "initWith" : "init";
    string selector = methodMeta.getSelector().substr(initPrefix.length(), string::npos);
    
    if (selector.length() > 0) {
      vector<string> ctorTokens;
      StringUtils::split(selector, ':', back_inserter(ctorTokens));
      
      // make the first letter of all tokens a lowercase letter
      for (vector<string>::size_type i = 0; i < ctorTokens.size(); i++) {
        string& token = ctorTokens[i];
        
        // ytho
        if (token == "ContentsOfURL") {
          token = "contentsOf";
        }

        for (vector<string>::size_type j = 0; j < token.size(); j++) {
          if (j == 0 || j == token.size() - 1) {
            token[j] = tolower(token[j]);
          }
          else if (j < token.size() - 1) {
            if (isupper(token[j+1])) {
              // next character is uppercase
              token[j] = tolower(token[j]);
            }
          }
        }
        
        if (token.size() >= 4 && token.substr(token.size() - 3, 3) == "url") {
          // *url -> *URL
          regex re("(\\w+)url$");
          token = regex_replace(token, re, "$1URL");
        }
        else if (token.size() >= 3 && token.substr(token.size() - 2, 2) == "id") {
          // *id -> *ID
          regex re("(\\w+)id$");
          token = regex_replace(token, re, "$1ID");
        }
        else if (token.size() >= 10 && token.substr(token.size() - 9, 9) == "WithTests") {
          // *WithTests -> *With
          regex re("(\\w+)WithTests$");
          token = regex_replace(token, re, "$1With");
        }
        else if (token.size() >= 9 && token.substr(token.size() - 8, 8) == "WithTest") {
          // *WithTest -> *With
          regex re("(\\w+)WithTest$");
          token = regex_replace(token, re, "$1With");
        }

        if (token.size() >= 5 && token.substr(0, 4) == "objc") {
          // objc* -> objC*
          token[3] = toupper(token[3]);
        }
      }
      
      // if the last parameter is NSError**, remove the last selector token
      if (methodMeta.getFlags(MetaFlags::MethodHasErrorOutParameter)) {
        ctorTokens.pop_back();
      }
      
      if (ctorTokens.size() > 0) {
        // rename duplicated tokens by adding digit at the end of the token
        for (vector<string>::size_type i = 0; i < ctorTokens.size(); i++) {
          int occurrences = 0;
          for (vector<string>::size_type j = 0; j < i; j++) {
            if (ctorTokens[i] == ctorTokens[j])
              occurrences++;
          }
          if (occurrences > 0) {
            ctorTokens[i] += to_string(occurrences + 1);
          }
        }
        
        ostringstream joinedTokens;
        const char* delimiter = ":";
        copy(ctorTokens.begin(), ctorTokens.end(), ostream_iterator<string>(joinedTokens, delimiter));
        methodMeta.constructorTokens = ctorTokens;
      }
    }
  }
  
  if (method.isVariadic() && !isNullTerminatedVariadic)
    throw MetaCreationException(&methodMeta, "Method is variadic (and is not marked as nil terminated.).", false);
  
  // set MethodOwnsReturnedCocoaObject flag
  clang::ObjCMethodFamily methodFamily = method.getMethodFamily();
  switch (methodFamily) {
    case clang::ObjCMethodFamily::OMF_copy:
      //case clang::ObjCMethodFamily::OMF_init :
      //case clang::ObjCMethodFamily::OMF_alloc :
    case clang::ObjCMethodFamily::OMF_mutableCopy:
    case clang::ObjCMethodFamily::OMF_new: {
      bool hasNsReturnsNotRetainedAttr = method.hasAttr<clang::NSReturnsNotRetainedAttr>();
      bool hasCfReturnsNotRetainedAttr = method.hasAttr<clang::CFReturnsNotRetainedAttr>();
      methodMeta.setFlags(MetaFlags::MethodOwnsReturnedCocoaObject, !(hasNsReturnsNotRetainedAttr || hasCfReturnsNotRetainedAttr));
      break;
    }
    default: {
      bool hasNsReturnsRetainedAttr = method.hasAttr<clang::NSReturnsRetainedAttr>();
      bool hasCfReturnsRetainedAttr = method.hasAttr<clang::CFReturnsRetainedAttr>();
      methodMeta.setFlags(MetaFlags::MethodOwnsReturnedCocoaObject, hasNsReturnsRetainedAttr || hasCfReturnsRetainedAttr);
      break;
    }
  }
  
  // set signature
  methodMeta.signature.push_back(method.hasRelatedResultType() ? _typeFactory.getInstancetype().get() : _typeFactory.create(method.getReturnType()).get());
  for (clang::ParmVarDecl* param : method.parameters()) {
    methodMeta.signature.push_back(_typeFactory.create(param->getType()).get());
  }
  
  bool returnsSelf = isInitializer || methodMeta.signature[0]->is(TypeInstancetype);
  
  methodMeta.setFlags(MetaFlags::MethodReturnsSelf, returnsSelf);
}

void MetaFactory::createFromProperty(const clang::ObjCPropertyDecl& property, PropertyMeta& propertyMeta)
{
  populateMetaFields(property, propertyMeta);
  
  propertyMeta.setFlags(MetaFlags::MemberIsOptional, property.isOptional());
  
  clang::ObjCMethodDecl* getter = property.getGetterMethodDecl();
  propertyMeta.getter = getter ? &create(*getter)->as<MethodMeta>() : nullptr;
  
  clang::ObjCMethodDecl* setter = property.getSetterMethodDecl();
  propertyMeta.setter = setter ? &create(*setter)->as<MethodMeta>() : nullptr;
}


// Objective-C runtime APIs (e.g. `class_getName` and similar) return the demangled
// names of Swift classes. Searching in metadata doesn't work if we keep the mangled ones.
string demangleSwiftName(string name) {
  // Start a long running `swift demangle` process in interactive mode.
  // Use `script` to force a PTY as suggested in https://unix.stackexchange.com/a/61833/347331
  // Otherwise, `swift demange` starts bufferring its stdout when it discovers that its not
  // in an interactive terminal.
  using namespace redi;
  static const string cmd = "script -q /dev/null xcrun swift demangle";
  static pstream ps(cmd, pstreams::pstdin|pstreams::pstdout|pstreams::pstderr);
  
  // Send the name to child process
  ps << name << endl;
  
  string result;
  // `script` prints both the input and output. Discard the input.
  getline(ps.out(), result);
  // Read the demangled name
  getline(ps.out(), result);
  // Strip any trailing whitespace
  result.erase(find_if(result.rbegin(), result.rend(), [](int ch) {
    return !isspace(ch);
  }).base(), result.end());
  
  return result;
}

vector<string> selectorParts(string selector) {
  vector<string> parts;
  
  if (selector == "") { return parts; }
  
  vector<string> outerTokens;
  vector<string> innerTokens;
  
  StringUtils::split(selector, '(', back_inserter(outerTokens));
  
  string rest;
  
  if (outerTokens.size() > 1) {
    // insertItem(withObjectValue:at:) -> ["insertItem", "withObjectValue", "at"]
    parts.push_back(outerTokens[0]);
    rest = outerTokens[1];
  }
  else {
    // insertItemWithObjectValue:atIndex: -> ["insertItemWithObjectValue", "atIndex"]
    rest = outerTokens[0];
  }
  
  StringUtils::split(rest, ':', back_inserter(innerTokens));
  
  for (auto innerToken: innerTokens) {
    if (innerToken != ")") {
      parts.push_back(innerToken);
    }
  }
  
  return parts;
}

void MetaFactory::populateIdentificationFields(const clang::NamedDecl& decl, Meta& meta, string categoryName)
{
  meta.declaration = &decl;
  
  // calculate name
  clang::ObjCRuntimeNameAttr* objCRuntimeNameAttribute = decl.getAttr<clang::ObjCRuntimeNameAttr>();
  
  if (objCRuntimeNameAttribute) {
    meta.name = objCRuntimeNameAttribute->getMetadataName().str();
    auto demangled = demangleSwiftName(meta.name);
    if (meta.name != demangled) {
      meta.demangledName = demangled;
    }
  } else {
    meta.name = decl.getNameAsString();
  }
  
  // calculate file name and module
  clang::SourceLocation location = _sourceManager.getFileLoc(decl.getLocation());
  clang::FileID fileId = _sourceManager.getDecomposedLoc(location).first;
  const clang::FileEntry* entry = _sourceManager.getFileEntryForID(fileId);
  if (entry != nullptr) {
    meta.fileName = entry->getName();
    meta.module = _headerSearch.findModuleForHeader(entry).getModule();
  }
  
  string nameKey;
  
  // calculate js name
  switch (decl.getKind()) {
    case clang::Decl::Kind::ObjCInterface:
    case clang::Decl::Kind::ObjCProtocol: {
      meta.jsName = decl.getNameAsString();
      nameKey = meta.jsName;
      break;
    }
    case clang::Decl::Kind::ObjCCategory:
    case clang::Decl::Kind::Function:
    case clang::Decl::Kind::Var:
    case clang::Decl::Kind::EnumConstant: {
      meta.jsName = decl.getNameAsString();
      nameKey = meta.jsName;
      break;
    }
    case clang::Decl::Kind::ObjCProperty: {
      const clang::ObjCPropertyDecl* property = clang::dyn_cast<clang::ObjCPropertyDecl>(&decl);
      meta.jsName = property->getNameAsString();
      nameKey = meta.jsName;
      break;
    }
    case clang::Decl::Kind::ObjCMethod: {
      const clang::ObjCMethodDecl* method = clang::dyn_cast<clang::ObjCMethodDecl>(&decl);
      meta.jsName = method->getNameAsString();
      nameKey = meta.name;
      break;
    }
    case clang::Decl::Kind::Record:
    case clang::Decl::Kind::Enum: {
      const clang::TagDecl* tagDecl = clang::dyn_cast<clang::TagDecl>(&decl);
      meta.name = meta.jsName = getTypedefOrOwnName(tagDecl);
      nameKey = meta.jsName;
      break;
    }
    default:
      throw logic_error(string("Can't generate jsName for ") + decl.getDeclKindName() + " type of declaration.");
  }

  // check if renamed
  if (nameKey.size() && meta.name.size()) {
    string selector;
    string renamed = renamedName(nameKey, categoryName);

    if (renamed != nameKey) {
      meta.isRenamed = true;
    }
    
    selector = renamed;

    vector<string> nameParts = selectorParts(selector);
    meta.jsName = nameParts[0];
    nameParts.erase(nameParts.begin());
    meta.argLabels = nameParts;
  }
  
  // We allow anonymous categories to be created. There is no need for categories to be named
  // because we don't keep them as separate entity in metadata. They are merged in their interfaces
  if (!meta.is(MetaType::Category)) {
    if (meta.fileName == "") {
      throw MetaCreationException(&meta, "", true);
    }
    else if (meta.module == nullptr) {
      throw MetaCreationException(&meta, "", false);
    } else if (meta.jsName == "") {
      throw MetaCreationException(&meta, "", false);
    }
  }
}

void MetaFactory::populateMetaFields(const clang::NamedDecl& decl, Meta& meta)
{
  if (!Utils::isMacOSBuild) {
    return;
  }
  
  vector<clang::AvailabilityAttr*> availabilityAttributes = Utils::getAttributes<clang::AvailabilityAttr>(decl);
  
  for (clang::AvailabilityAttr* availability : availabilityAttributes) {
    string platform = availability->getPlatform()->getName().str();
    
    if (platform != string("macos") && platform != string("swift")) {
      continue;
    }
    
    if (availability->getUnavailable()) {
      meta.unavailable = true;
    }
    
    meta.introducedIn = this->convertVersion(availability->getIntroduced());
    meta.deprecatedIn = this->convertVersion(availability->getDeprecated());
    meta.obsoletedIn = this->convertVersion(availability->getObsoleted());
  }
  
  if (meta.deprecatedIn.Major > 0 && Utils::buildTarget.isGreaterThanOrEqualOrUnknown(meta.deprecatedIn)) {
    throw MetaCreationException(&meta, "", false);
  }
  
  if (meta.obsoletedIn.Major > 0 && Utils::swiftVersion.isGreaterThanOrEqualOrUnknown(meta.obsoletedIn)) {
    throw MetaCreationException(&meta, "", false);
  }
  
  if (meta.introducedIn.Major > 0 && meta.introducedIn.isGreaterThanOrUnknown(Utils::buildTarget)) {
    throw MetaCreationException(&meta, "", false);
  }
}

void MetaFactory::populateBaseClassMetaFields(const clang::ObjCContainerDecl& decl, BaseClassMeta& baseClass)
{
  const clang::ObjCCategoryDecl* category = clang::dyn_cast<clang::ObjCCategoryDecl>(&decl);
  string categoryName = "";
  
  if (category != NULL) {
    categoryName = category->getClassInterface()->getNameAsString();
  }
  else {
    categoryName = decl.getNameAsString();
  }
  
  categoryName = renamedName(categoryName);
  
  for (clang::ObjCProtocolDecl* protocol : this->getProtocols(&decl)) {
    Meta* protocolMeta;
    
    if (protocol->getDefinition() != nullptr && this->tryCreate(*protocol->getDefinition(), &protocolMeta, categoryName)) {
      if (protocol->isUnavailable() || protocolMeta->unavailable) {
        continue;
      }
      baseClass.protocols.push_back(&protocolMeta->as<ProtocolMeta>());
    }
  }
  sort(baseClass.protocols.begin(), baseClass.protocols.end(), metasComparerByJsName); // order by jsName
  
  for (clang::ObjCMethodDecl* classMethod : decl.class_methods()) {
    Meta* methodMeta;
    if (!classMethod->isImplicit() && this->tryCreate(*classMethod, &methodMeta, categoryName)) {
      if (classMethod->isUnavailable() || methodMeta->unavailable) {
        continue;
      }
      baseClass.staticMethods.push_back(&methodMeta->as<MethodMeta>());
    }
  }
  
  sort(baseClass.staticMethods.begin(), baseClass.staticMethods.end(), metasComparerByJsName); // order by jsName
  
  for (clang::ObjCMethodDecl* instanceMethod : decl.instance_methods()) {
    Meta* methodMeta;
    if (!instanceMethod->isImplicit() && this->tryCreate(*instanceMethod, &methodMeta, categoryName)) {
      if (instanceMethod->isUnavailable() || methodMeta->unavailable) {
        continue;
      }
      baseClass.instanceMethods.push_back(&methodMeta->as<MethodMeta>());
    }
  }
  sort(baseClass.instanceMethods.begin(), baseClass.instanceMethods.end(), metasComparerByJsName); // order by jsName
  
  for (clang::ObjCPropertyDecl* property : decl.properties()) {
    Meta* propertyMeta;
    if (this->tryCreate(*property, &propertyMeta, categoryName)) {
      if (propertyMeta->unavailable) {
        continue;
      }
      if (!property->isClassProperty()) {
        baseClass.instanceProperties.push_back(&propertyMeta->as<PropertyMeta>());
      } else {
        baseClass.staticProperties.push_back(&propertyMeta->as<PropertyMeta>());
      }
    }
  }
  sort(baseClass.instanceProperties.begin(), baseClass.instanceProperties.end(), metasComparerByJsName); // order by jsName
  sort(baseClass.staticProperties.begin(), baseClass.staticProperties.end(), metasComparerByJsName); // order by jsName
}

string MetaFactory::renameMeta(MetaType type, string& originalJsName, int index)
{
  string indexStr = index == 1 ? "" : to_string(index);
  switch (type) {
    case MetaType::Interface:
      return originalJsName + "Interface" + indexStr;
    case MetaType::Protocol:
      return originalJsName + "Protocol" + indexStr;
    case MetaType::Function:
      return originalJsName + "Function" + indexStr;
    case MetaType::Var:
      return originalJsName + "Var" + indexStr;
    case MetaType::Struct:
      return originalJsName + "Struct" + indexStr;
    case MetaType::Union:
      return originalJsName + "Union" + indexStr;
    case MetaType::Enum:
      return originalJsName + "Enum" + indexStr;
    case MetaType::EnumConstant:
      return originalJsName + "Var" + indexStr;
    case MetaType::Method:
      return originalJsName + "Method" + indexStr;
    default:
      return originalJsName + "Decl" + indexStr;
  }
}

llvm::iterator_range<clang::ObjCProtocolList::iterator> MetaFactory::getProtocols(const clang::ObjCContainerDecl* objCContainer)
{
  if (const clang::ObjCInterfaceDecl* interface = clang::dyn_cast<clang::ObjCInterfaceDecl>(objCContainer))
    return interface->protocols();
  else if (const clang::ObjCProtocolDecl* protocol = clang::dyn_cast<clang::ObjCProtocolDecl>(objCContainer))
    return protocol->protocols();
  else if (const clang::ObjCCategoryDecl* category = clang::dyn_cast<clang::ObjCCategoryDecl>(objCContainer))
    return category->protocols();
  throw logic_error("Unable to extract protocols form this type of ObjC container.");
}

Version MetaFactory::convertVersion(clang::VersionTuple clangVersion)
{
  Version result = {
    .Major = (int)clangVersion.getMajor(),
    .Minor = (int)(clangVersion.getMinor().hasValue() ? clangVersion.getMinor().getValue() : -1),
    .SubMinor = (int)(clangVersion.getSubminor().hasValue() ? clangVersion.getSubminor().getValue() : -1)
  };
  return result;
}
}
