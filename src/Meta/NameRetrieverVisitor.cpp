//
//  NameRetrieverVisitor.cpp
//  objc-metadata-generator
//
//  Created by Martin Bekchiev on 3.09.18.
//

#include "NameRetrieverVisitor.h"
#include "MetaEntities.h"

#include <sstream>

using namespace std;

NameRetrieverVisitor NameRetrieverVisitor::instanceObjC(false);
NameRetrieverVisitor NameRetrieverVisitor::instanceTs(true);

string NameRetrieverVisitor::visitVoid() {
    return "void";
}
    
string NameRetrieverVisitor::visitBool() {
    return this->tsNames ? "boolean" : "bool";
}
    
string NameRetrieverVisitor::visitShort() {
    return this->tsNames ? "number" : "short";
}

string NameRetrieverVisitor::visitUShort() {
    return this->tsNames ? "number" : "unsigned short";
}
    
string NameRetrieverVisitor::visitInt() {
    return this->tsNames ? "number" : "int";
}
    
string NameRetrieverVisitor::visitUInt() {
    return this->tsNames ? "number" : "unsigned int";
}
    
string NameRetrieverVisitor::visitLong() {
    return this->tsNames ? "number" : "long";
}
    
string NameRetrieverVisitor::visitUlong() {
    return this->tsNames ? "number" : "unsigned long";
}
    
string NameRetrieverVisitor::visitLongLong() {
    return this->tsNames ? "number" : "long long";
}
    
string NameRetrieverVisitor::visitULongLong() {
    return this->tsNames ? "number" : "unsigned long long";
}
    
string NameRetrieverVisitor::visitSignedChar() {
    return this->tsNames ? "number" : "signed char";
}
    
string NameRetrieverVisitor::visitUnsignedChar() {
    return this->tsNames ? "number" : "unsigned char";
}
    
string NameRetrieverVisitor::visitUnichar() {
    return this->tsNames ? "number" : "wchar_t";
}
    
string NameRetrieverVisitor::visitCString() {
    return this->tsNames ? "string" : "char*";
}
    
string NameRetrieverVisitor::visitFloat() {
    return this->tsNames ? "number" : "float";
}
    
string NameRetrieverVisitor::visitDouble() {
    return this->tsNames ? "number" : "double";
}
    
string NameRetrieverVisitor::visitVaList() {
    return "";
}
    
string NameRetrieverVisitor::visitSelector() {
    return this->tsNames ? "string" : "SEL";
}
    
string NameRetrieverVisitor::visitInstancetype() {
    return this->tsNames ? "any" : "instancetype";
}
    
string NameRetrieverVisitor::visitClass(const ClassType& typeDetails) {
    return this->tsNames ? "any" : "Class";
}
    
string NameRetrieverVisitor::visitProtocol() {
    return this->tsNames ? "any" : "Protocol";
}
    
string NameRetrieverVisitor::visitId(const IdType& typeDetails) {
    return this->tsNames ? "any" : "id";
}
    
string NameRetrieverVisitor::visitConstantArray(const ConstantArrayType& typeDetails) {
    return this->generateFixedArray(typeDetails.innerType, typeDetails.size);
}
    
string NameRetrieverVisitor::visitExtVector(const ExtVectorType& typeDetails) {
    return this->generateFixedArray(typeDetails.innerType, typeDetails.size);
}

string NameRetrieverVisitor::visitIncompleteArray(const IncompleteArrayType& typeDetails) {
    return typeDetails.innerType->visit(*this).append("[]");
}
    
string NameRetrieverVisitor::visitInterface(const InterfaceType& typeDetails) {
    return this->tsNames ? typeDetails.interface->jsName : typeDetails.interface->name;
}
    
string NameRetrieverVisitor::visitBridgedInterface(const BridgedInterfaceType& typeDetails) {
    return typeDetails.name;
}
    
string NameRetrieverVisitor::visitPointer(const PointerType& typeDetails) {
    return this->tsNames ? "any" : typeDetails.innerType->visit(*this).append("*");
}
    
string NameRetrieverVisitor::visitBlock(const BlockType& typeDetails) {
    return this->tsNames ? this->getFunctionTypeScriptName(typeDetails.signature) : "void*" /*TODO: construct objective-c full block definition*/;
}

string NameRetrieverVisitor::visitFunctionPointer(const FunctionPointerType& typeDetails) {
    return this->tsNames ? this->getFunctionTypeScriptName(typeDetails.signature) : "void*"/*TODO: construct objective-c full function pointer definition*/;
}

string NameRetrieverVisitor::visitStruct(const StructType& typeDetails) {
    return this->tsNames ? typeDetails.structMeta->jsName : typeDetails.structMeta->name;
}
    
string NameRetrieverVisitor::visitUnion(const UnionType& typeDetails) {
    return this->tsNames ? typeDetails.unionMeta->jsName : typeDetails.unionMeta->name;
}
    
string NameRetrieverVisitor::visitAnonymousStruct(const AnonymousStructType& typeDetails) {
    return "";
}
    
string NameRetrieverVisitor::visitAnonymousUnion(const AnonymousUnionType& typeDetails) {
    return "";
}
    
string NameRetrieverVisitor::visitEnum(const EnumType& typeDetails) {
    return this->tsNames ? typeDetails.enumMeta->jsName : typeDetails.enumMeta->name;
}
    
string NameRetrieverVisitor::visitTypeArgument(const ::Meta::TypeArgumentType& type) {
    return type.name;
}

string NameRetrieverVisitor::generateFixedArray(const Type *el_type, size_t size) {
    stringstream ss(el_type->visit(*this));
    ss << "[";
    if (!this->tsNames) {
        ss << size;
    }
    ss << "]";
    
    return ss.str();
}

string NameRetrieverVisitor::getFunctionTypeScriptName(const vector<Type*> &signature) {
    // (p1: t1,...) => ret_type
    assert(signature.size() > 0);
    
    stringstream ss;
    ss << "(";
    for (size_t i = 1; i < signature.size(); i++) {
        if (i > 1) {
            ss << ", ";
        }
        ss << "p" << i << ": " << signature[i]->visit(*this);
    }
    ss << ")";
    ss << " => ";
    ss << signature[0]->visit(*this);
    
    return ss.str();
}

