//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenShader/Nodes/CompoundNode.h>
#include <MaterialXGenShader/ColorManagementSystem.h>
#include <MaterialXGenShader/Exception.h>
#include <MaterialXGenShader/ShaderGenerator.h>
#include <MaterialXGenShader/Util.h>

#include <MaterialXCore/Definition.h>
#include <MaterialXCore/Document.h>
#include <MaterialXCore/Library.h>

#include <MaterialXTrace/Tracing.h>

MATERIALX_NAMESPACE_BEGIN

ShaderNodeImplPtr CompoundNode::create()
{
    return std::make_shared<CompoundNode>();
}

string CompoundNode::getColorSpaceSignature(const NodeDef& nodeDef, GenContext& context)
{
    // The signature is computed from the node instance currently being created,
    // which is the innermost node on the context's parent node stack.
    const vector<ConstNodePtr>& parentNodes = context.getParentNodes();
    if (parentNodes.empty())
    {
        return EMPTY_STRING;
    }
    ConstNodePtr node = parentNodes.back();
    if (!node || node->getNodeDef().get() != &nodeDef)
    {
        return EMPTY_STRING;
    }
    if (!node->isColorType() && !node->isMultiOutputType())
    {
        return EMPTY_STRING;
    }

    // Determine the target color space, matching the logic in ShaderGraph.
    const string& targetColorSpace = context.getOptions().targetColorSpaceOverride.empty() ?
                                     node->getDocument()->getActiveColorSpace() :
                                     context.getOptions().targetColorSpaceOverride;
    if (targetColorSpace.empty())
    {
        return EMPTY_STRING;
    }

    ColorManagementSystemPtr colorManagementSystem = context.getShaderGenerator().getColorManagementSystem();

    // Collect the filename inputs that require a color space transform.
    string signature;
    for (InputPtr input : node->getInputs())
    {
        if (input->getType() != FILENAME_TYPE_STRING || (!input->hasValue() && !input->hasInterfaceName()))
        {
            continue;
        }
        const string sourceColorSpace = getFileNameColorSpace(input, parentNodes);
        if (sourceColorSpace.empty())
        {
            continue;
        }
        // Match the no-op detection in ShaderGraph::populateColorTransformMap.
        const bool isNoOpTransform = colorManagementSystem ?
                                     colorManagementSystem->isNoOpTransform(sourceColorSpace, targetColorSpace) :
                                     sourceColorSpace == targetColorSpace ||
                                     ColorManagementSystem::isReservedNoOpColorSpace(sourceColorSpace) ||
                                     ColorManagementSystem::isReservedNoOpColorSpace(targetColorSpace);
        if (isNoOpTransform)
        {
            continue;
        }
        signature += "_" + input->getName() + "_" + sourceColorSpace;
    }
    return signature;
}

void CompoundNode::addClassification(ShaderNode& node) const
{
    // Add classification from the graph implementation.
    node.addClassification(_rootGraph->getClassification());
}

void CompoundNode::initialize(const InterfaceElement& element, GenContext& context)
{
    MX_TRACE_FUNCTION(Tracing::Category::ShaderGen);
    MX_TRACE_SCOPE(Tracing::Category::ShaderGen, element.getName().c_str());

    ShaderNodeImpl::initialize(element, context);

    if (!element.isA<NodeGraph>())
    {
        throw ExceptionShaderGenError("Element '" + element.getName() + "' is not a node graph implementation");
    }

    const NodeGraph& graph = static_cast<const NodeGraph&>(element);

    _functionName = graph.getName();

    // Instances with different color space declarations on their filename inputs
    // get dedicated implementations of this graph, with color space transforms
    // applied per texture inside the graph, so give each variant a unique
    // function name.
    ConstNodeDefPtr nodeDef = graph.getNodeDef();
    if (nodeDef)
    {
        _functionName += getColorSpaceSignature(*nodeDef, context);
    }
    context.getShaderGenerator().getSyntax().makeValidName(_functionName);

    // For compounds we do not want to publish all internal inputs
    // so always use the reduced interface for this graph.
    const ShaderInterfaceType oldShaderInterfaceType = context.getOptions().shaderInterfaceType;
    context.getOptions().shaderInterfaceType = SHADER_INTERFACE_REDUCED;
    _rootGraph = ShaderGraph::create(nullptr, graph, context);
    context.getOptions().shaderInterfaceType = oldShaderInterfaceType;

    // Set hash using the function name.
    // TODO: Could be improved to include the full function signature.
    _hash = std::hash<string>{}(_functionName);
}

void CompoundNode::createVariables(const ShaderNode&, GenContext& context, Shader& shader) const
{
    // Gather shader inputs from all child nodes
    for (const ShaderNode* childNode : _rootGraph->getNodes())
    {
        childNode->getImplementation().createVariables(*childNode, context, shader);
    }
}

void CompoundNode::emitFunctionDefinition(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    MX_TRACE_FUNCTION(Tracing::Category::ShaderGen);
    MX_TRACE_SCOPE(Tracing::Category::ShaderGen, _functionName.c_str());

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        const ShaderGenerator& shadergen = context.getShaderGenerator();

        // Emit functions for all child nodes
        shadergen.emitFunctionDefinitions(*_rootGraph, context, stage);

        // Begin function signature.
        shadergen.emitLineBegin(stage);
        shadergen.emitString("void " + _functionName + "(", stage);

        shadergen.emitClosureDataParameter(node, context, stage);
        // if (context.getShaderGenerator().nodeNeedsClosureData(node))
        // {
        //     shadergen.emitString(HW::CLOSURE_DATA_TYPE + " " + HW::CLOSURE_DATA_ARG + ", ", stage);
        // }

        string delim;

        // Add all inputs
        for (ShaderGraphInputSocket* inputSocket : _rootGraph->getInputSockets())
        {
            shadergen.emitString(delim, stage);
            shadergen.emitFunctionDefinitionParameter(inputSocket, false, context, stage);
            delim = ", ";
        }

        // Add all outputs
        for (ShaderGraphOutputSocket* outputSocket : _rootGraph->getOutputSockets())
        {
            shadergen.emitString(delim, stage);
            shadergen.emitFunctionDefinitionParameter(outputSocket, true, context, stage);
            delim = ", ";
        }

        // End function signature.
        shadergen.emitString(")", stage);
        shadergen.emitLineEnd(stage, false);

        // Begin function body.
        shadergen.emitFunctionBodyBegin(*_rootGraph, context, stage);

        if (nodeOutputIsClosure(node))
        {
            // Emit all texturing nodes. These are inputs to the
            // closure nodes and need to be emitted first.
            shadergen.emitFunctionCalls(*_rootGraph, context, stage, ShaderNode::Classification::TEXTURE);

            // Emit function calls for internal closures nodes connected to the graph sockets.
            // These will in turn emit function calls for any dependent closure nodes upstream.
            for (ShaderGraphOutputSocket* outputSocket : _rootGraph->getOutputSockets())
            {
                if (outputSocket->getConnection())
                {
                    const ShaderNode* upstream = outputSocket->getConnection()->getNode();
                    // Its important that the classification check here matches the logic inside
                    // nodeOutputIsClosure() used above.
                    if (upstream->getParent() == _rootGraph.get() &&
                        (upstream->hasClassification(ShaderNode::Classification::CLOSURE) ||
                            upstream->hasClassification(ShaderNode::Classification::SHADER) ||
                            upstream->hasClassification(ShaderNode::Classification::MATERIAL)))
                    {
                        shadergen.emitFunctionCall(*upstream, context, stage);
                    }
                }
            }
        }
        else
        {
            shadergen.emitFunctionCalls(*_rootGraph, context, stage);
        }

        // Emit final results
        for (ShaderGraphOutputSocket* outputSocket : _rootGraph->getOutputSockets())
        {
            const string result = shadergen.getUpstreamResult(outputSocket, context);
            shadergen.emitLine(outputSocket->getVariable() + " = " + result, stage);
        }

        // End function body.
        shadergen.emitFunctionBodyEnd(*_rootGraph, context, stage);
    }
}

void CompoundNode::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    MX_TRACE_FUNCTION(Tracing::Category::ShaderGen);
    MX_TRACE_SCOPE(Tracing::Category::ShaderGen, _functionName.c_str());

    const ShaderGenerator& shadergen = context.getShaderGenerator();

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        // Emit function calls for all child nodes to the vertex shader stage
        shadergen.emitFunctionCalls(*_rootGraph, context, stage);
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        if (nodeOutputIsClosure(node))
        {
            // Emit calls for any closure dependencies upstream from this nodedef
            shadergen.emitDependentFunctionCalls(node, context, stage, ShaderNode::Classification::CLOSURE);
        }

        // Declare the output variables.
        emitOutputVariables(node, context, stage);

        // Begin function call.
        shadergen.emitLineBegin(stage);
        shadergen.emitString(_functionName + "(", stage);

        // Add an argument for closure data if needed
        shadergen.emitClosureDataArg(node, context, stage);
        // if (context.getShaderGenerator().nodeNeedsClosureData(node))
        // {
        //     shadergen.emitString(HW::CLOSURE_DATA_ARG + ", ", stage);
        // }

        string delim;

        // Emit inputs.
        for (ShaderInput* input : node.getInputs())
        {
            shadergen.emitString(delim, stage);
            shadergen.emitInput(input, context, stage);
            delim = ", ";
        }

        // Emit outputs.
        for (size_t i = 0; i < node.numOutputs(); ++i)
        {
            shadergen.emitString(delim, stage);
            shadergen.emitOutput(node.getOutput(i), false, false, context, stage);
            delim = ", ";
        }

        // End function call
        shadergen.emitString(")", stage);
        shadergen.emitLineEnd(stage);
    }
}

MATERIALX_NAMESPACE_END
