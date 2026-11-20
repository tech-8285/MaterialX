//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_SHADERTRANSLATOR_H
#define MATERIALX_SHADERTRANSLATOR_H

#include <MaterialXGenShader/Export.h>

#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

MATERIALX_NAMESPACE_BEGIN

/// A shared pointer to a ShaderTranslator
using ShaderTranslatorPtr = shared_ptr<class ShaderTranslator>;

/// @class ShaderTranslator
/// A helper class for translating content between shading models.
class MX_GENSHADER_API ShaderTranslator
{
  public:
    static ShaderTranslatorPtr create()
    {
        return ShaderTranslatorPtr(new ShaderTranslator());
    }

    /// Translate a shader node to the destination shading model.
    ///
    /// Upstream connections may originate in any number of node graphs, from
    /// nodes at the document's top level, or from specific outputs of
    /// multi-output nodes, and each connection is preserved on the matching
    /// target input.  Inputs whose upstream data is expressed in world space
    /// (such as the output of a "normalmap" node) are not routed through the
    /// translation graph, whose scalar math assumes tangent-space data; the
    /// world-space node is instead recreated outside that graph and wired
    /// directly to the corresponding target shader input.
    ///
    /// If translation fails, then the document containing the shader is
    /// rolled back to its state before the call and remains usable.
    void translateShader(NodePtr shader, const string& destCategory);

    /// Translate each material in the input document to the destination
    /// shading model.  If translation of any material fails, then the input
    /// document is restored to its state before the call and remains usable.
    void translateAllMaterials(DocumentPtr doc, const string& destShader);

  protected:
    ShaderTranslator() = default;

    // Connect translation node inputs from the original shader.
    void connectTranslationInputs(NodePtr shader, NodeDefPtr translationNodeDef);

    // Connect translation node outputs to finalize shader translation.
    void connectTranslationOutputs(NodePtr shader);

    // Undo every modification recorded during the current translation.
    void rollback();

    // Store a deep copy of the given element in the scratch document used
    // for rollback, returning the copied element.
    ElementPtr stashElement(ConstElementPtr element);

  protected:
    NodeGraphPtr _graph;
    NodePtr _translationNode;

    // A scratch document that owns deep copies of the elements removed
    // during the current translation, allowing them to be restored.
    DocumentPtr _scratchDoc;

    // Elements created during the current translation, removed in reverse
    // order when the translation is rolled back.
    vector<ElementPtr> _createdElements;

    // Outputs removed from the translation graph, paired with the parent
    // graph from which they were removed and with the stashed deep copy
    // used to restore them.
    struct RemovedOutput
    {
        NodeGraphPtr parent;
        string originalName;
        ElementPtr copy;
    };
    vector<RemovedOutput> _removedOutputs;

    // The shader inputs removed during translation, paired with the shader
    // from which they were removed and with the stashed deep copy used to
    // restore them.
    struct RemovedShaderInput
    {
        NodePtr shader;
        string originalName;
        ElementPtr copy;
    };
    vector<RemovedShaderInput> _removedShaderInputs;

    // The original category and nodedef string of the translated shader,
    // restored on rollback.
    string _originalCategory;
    string _originalNodeDefString;

    // Names of shader inputs whose upstream connection produces world-space
    // coordinates, mapped to the world-space node found upstream.
    std::map<string, NodePtr> _worldSpaceInputs;
};

MATERIALX_NAMESPACE_END

#endif
