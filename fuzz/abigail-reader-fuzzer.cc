#include <stdexcept>
#include <string>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include "abigail-reader.h"

static void DoNothing(void*, const char*, ...) {}

extern "C" int LLVMFuzzerTestOneInput(char* data, size_t size) {
  xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
  // Suppress libxml error messages.
  xmlSetGenericErrorFunc(ctxt, (xmlGenericErrorFunc) DoNothing);
  xmlDocPtr doc = xmlCtxtReadMemory(ctxt, data, size, nullptr, nullptr,
                                    XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  xmlFreeParserCtxt(ctxt);

  // Bail out if the doc XML is invalid.
  if (!doc)
    return 0;

  xmlNodePtr root = xmlDocGetRootElement(doc);
  if (root) {
    try {
      stg::abixml::Abigail _(root);
    } catch (const stg::abixml::AbigailReaderException&) {
      // Pass as this is us catching invalid XML properly.
    }
  }

  xmlFreeDoc(doc);

  return 0;
}
