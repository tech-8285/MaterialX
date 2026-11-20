//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_COMPOUNDNODE_H
#define MATERIALX_COMPOUNDNODE_H

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/ShaderNodeImpl.h>
#include <MaterialXGenShader/ShaderGraph.h>
#include <MaterialXGenShader/Shader.h>

MATERIALX_NAMESPACE_BEGIN

/// Compound node implementation
class MX_GENSHADER_API CompoundNode : public ShaderNodeImpl
{
  public:
    static ShaderNodeImplPtr create();

    void initialize(const InterfaceElement& element, GenContext& context) override;

    void addClassification(ShaderNode& node) const override;

    void createVariables(const ShaderNode& node, GenContext& context, Shader& shader) const override;

    void emitFunctionDefinition(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;

    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;

    ShaderGraph* getGraph() const override { return _rootGraph.get(); }

    /// Compute a signature string identifying the color space configuration of the
    /// filename inputs on the compound node instance currently being created, i.e.
    /// the innermost node on the context's parent node stack. Returns an empty string
    /// if the innermost parent node is not an instance of the given nodedef, or if
    /// none of its filename inputs require a color space transform. Compound node
    /// instances with different signatures require dedicated implementations, since
    /// color space transforms are applied per texture inside the node graph.
    static string getColorSpaceSignature(const NodeDef& nodeDef, GenContext& context);

  protected:
    ShaderGraphPtr _rootGraph;
    string _functionName;
};

MATERIALX_NAMESPACE_END

#endif
