#include "JSExportDefinitionWriter.h"
#include "Meta/Utils.h"
#include "Meta/MetaFactory.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>
#include <regex>
#include "yaml-cpp/yaml.h"
#include "JSExportMeta.h"

namespace TypeScript {
using namespace Meta;
using namespace std;

JSExportMeta JSExportMeta::current = JSExportMeta();

string JSExportMeta::outputJSEFolder = "";
}
