#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelCascade);
    p->addModel(modelDiscourse);
    p->addModel(modelPareto);
    p->addModel(modelDilemma);
    p->addModel(modelDiffusion);
    p->addModel(modelNetwork);
}
