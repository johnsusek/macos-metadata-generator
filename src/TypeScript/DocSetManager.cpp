//
// Created by Ivan Buhov on 11/6/15.
//

#include "DocSetManager.h"
#include <libxml/xpath.h>
#include <sstream>
#include <unistd.h>

namespace {
using namespace std;


bool isntspace(char ch) {
    return !isspace(ch);
}

// trim from start
string& ltrim(string& s)
{
    s.erase(s.begin(), find_if(s.begin(), s.end(), isntspace));
    return s;
}

// trim from end
string& rtrim(string& s)
{
    s.erase(find_if(s.rbegin(), s.rend(), isntspace).base(), s.end());
    return s;
}

// trim from both ends
string& trim(string& s)
{
    return ltrim(rtrim(s));
}

void findAndReplaceIn(string& str, string searchFor, string replaceBy)
{
    size_t found = str.find(searchFor);
    while (found != string::npos) {
        str.replace(found, searchFor.length(), replaceBy);
        found = str.find(searchFor, found + replaceBy.length());
    };
}

xmlNodeSetPtr all(const xmlChar* xpath, xmlDocPtr doc, xmlXPathObjectPtr& result)
{
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (xmlXPathNodeSetIsEmpty(result->nodesetval)) {
        xmlXPathFreeObject(result);
        result = nullptr;
        return NULL;
    }
    return result->nodesetval;
}

xmlNodePtr first(const xmlChar* xpath, xmlDocPtr doc, xmlXPathObjectPtr& result)
{
    xmlNodeSetPtr nodes = all(xpath, doc, result);
    if (nodes && nodes->nodeNr > 0)
        return nodes->nodeTab[0];
    return nullptr;
}

string innerTextOf(xmlNodePtr xmlNode, xmlDocPtr doc)
{
    char* innerText = reinterpret_cast<char*>(xmlNodeGetContent(xmlNode));
    string innerStr(innerText);
    xmlFree(innerText);
    return innerStr;
}

vector<string> innerTextOf(xmlNodeSetPtr xmlNodes, xmlDocPtr doc)
{
    vector<string> result;
    if (xmlNodes != nullptr) {
        for (int i = 0; i < xmlNodes->nodeNr; ++i) {
            result.push_back(innerTextOf(xmlNodes->nodeTab[i], doc));
        }
    }
    return result;
}
}

namespace TypeScript {
using namespace std;

string TSComment::toString(string linePrefix)
{
    if (description.length() == 0 && params.size() == 0) {
        return string();
    }

    stringstream result;
    result << linePrefix << "/**" << endl;
    string processedDesc = description;
    findAndReplaceIn(processedDesc, "\n", "");
    result << linePrefix << " * " << processedDesc << endl;
    for (pair<string, string>& param : params) {
        // @param paramName - paramDesc
        result << linePrefix << " * "
               << "@param " + param.first + " - " + param.second << endl;
    }
    result << linePrefix << " */" << endl;
    return result.str();
}

TSComment DocSetManager::getCommentFor(Meta::Meta* meta, Meta::Meta* parent)
{
    return (parent == nullptr) ? getCommentFor(meta->name, meta->type) : getCommentFor(meta->name, meta->type, parent->name, parent->type);
}

TSComment DocSetManager::getCommentFor(string name, Meta::MetaType type, string parentName, Meta::MetaType parentType)
{
    xmlDocPtr doc = getXmlDocFileFor(name, type, parentName, parentType);

    TSComment comment;
    if (doc) {
        xmlXPathObjectPtr abstractNodeResult = nullptr;
        xmlNodePtr abstractNode = first(reinterpret_cast<const xmlChar*>("/*/Abstract"), doc, abstractNodeResult);
        if (abstractNode != nullptr) {
            string description = innerTextOf(abstractNode, doc);
            comment.description = trim(description);
        }

        switch (type) {
        case Meta::MetaType::Method:
        case Meta::MetaType::Function: {
            xmlXPathObjectPtr termNodesResult = nullptr;
            vector<string> paramNames = innerTextOf(all(reinterpret_cast<const xmlChar*>("/*/Parameters/Parameter/Term"), doc, termNodesResult), doc);
            if (paramNames.size() > 0) {
                xmlXPathObjectPtr discussionNodesResult = nullptr;
                vector<string> paramDescs = innerTextOf(all(reinterpret_cast<const xmlChar*>("/*/Parameters/Parameter/Discussion"), doc, discussionNodesResult), doc);
                assert(paramNames.size() == paramDescs.size());

                for (size_t i = 0; i < paramNames.size(); i++) {
                    comment.params.push_back(pair<string, string>(paramNames[i], trim(paramDescs[i])));
                }
                xmlXPathFreeObject(discussionNodesResult);
            }
            xmlXPathFreeObject(termNodesResult);
            break;
        }
        case Meta::MetaType::Struct:
        case Meta::MetaType::Union: {
            xmlXPathObjectPtr discussionNodesResult = nullptr;
            vector<string> fieldsDescs = innerTextOf(all(reinterpret_cast<const xmlChar*>("/*/Fields/Field/Discussion"), doc, discussionNodesResult), doc);
            if (fieldsDescs.size() > 0) {
                for (size_t i = 0; i < fieldsDescs.size(); i++) {
                    TSComment fieldComment;
                    fieldComment.description = trim(fieldsDescs[i]);
                    comment.fields.push_back(fieldComment);
                }
            }
            xmlXPathFreeObject(discussionNodesResult);
            break;
        }
        default: {
            break;
        }
        }
        xmlXPathFreeObject(abstractNodeResult);
    }

    xmlFreeDoc(doc);
    return comment;
}

xmlDocPtr DocSetManager::getXmlDocFileFor(string name, Meta::MetaType type, string parentName, Meta::MetaType parentType)
{
    string parent = (parentName == "") ? "-" : parentName;
    vector<string> xmlPathCandidates;
    switch (type) {
    case Meta::MetaType::Struct: {
        xmlPathCandidates.push_back(this->tokensPath + "/c/tdef/" + parent + "/" + name + ".xml");
        xmlPathCandidates.push_back(this->tokensPath + "/c/tag/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Function: {
        xmlPathCandidates.push_back(this->tokensPath + "/c/func/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Enum: {
        xmlPathCandidates.push_back(this->tokensPath + "/c/tdef/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::EnumConstant: {
        xmlPathCandidates.push_back(this->tokensPath + "/c/econst/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Var: {
        xmlPathCandidates.push_back(this->tokensPath + "/c/data/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Interface: {
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/cl/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Protocol: {
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/intf/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Category: {
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/cat/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Method: {
        string type1 = (parentType == Meta::MetaType::Interface) ? "instm" : "intfm";
        string type2 = (parentType == Meta::MetaType::Interface) ? "clm" : "intfcm";
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/" + type1 + "/" + parent + "/" + name + ".xml");
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/" + type2 + "/" + parent + "/" + name + ".xml");
        break;
    }
    case Meta::MetaType::Property: {
        string type = (parentType == Meta::MetaType::Interface) ? "instp" : "intfp";
        xmlPathCandidates.push_back(this->tokensPath + "/Objective-C/" + type + "/" + parent + "/" + name + ".xml");
        break;
    }
    default: {
        break;
    }
    }

    for (string& path : xmlPathCandidates) {
        if (access(path.c_str(), F_OK) == 0) {
            return xmlReadFile(path.c_str(), nullptr, 0);
        }
    }
    return nullptr;
}
}
