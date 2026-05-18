#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelProspect);
    p->addModel(modelBandit);
    p->addModel(modelDDM);
}
