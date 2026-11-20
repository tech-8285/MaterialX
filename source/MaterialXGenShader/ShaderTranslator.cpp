//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGenShader/ShaderTranslator.h>

#include <MaterialXCore/Material.h>

MATERIALX_NAMESPACE_BEGIN

namespace
{

/// Return the node graph that directly owns the given node, or a null
/// pointer for nodes living at the document's top level.
NodeGraphPtr getOwningNodeGraph(NodePtr node)
{
    return node ? node->getParent()->asA<NodeGraph>() : nullptr;
}

/// Recreate an output on the given graph so that a node living there can be
/// referenced from another graph.  Existing outputs pointing at the same
/// node are reused.  When nodeOutputString is non-empty the node is
/// multi-output and the output selects one of its outputs; otherwise the
/// node has a single implicit output.
OutputPtr getOrCreateGraphOutput(NodeGraphPtr externalGraph, NodePtr upstreamNode,
                                 const string& nodeOutputString, vector<ElementPtr>& createdElements)
{
    for (OutputPtr existingOutput : externalGraph->getOutputs())
    {
        if (existingOutput->getConnectedNode() == upstreamNode &&
            existingOutput->getOutputString() == nodeOutputString)
        {
            return existingOutput;
        }
    }

    string outputName = upstreamNode->getName() + "_translation_output";
    if (!nodeOutputString.empty())
    {
        outputName += "_" + nodeOutputString;
    }
    outputName = externalGraph->createValidChildName(outputName);

    OutputPtr externalOutput = externalGraph->addOutput(outputName, upstreamNode->getType());
    externalOutput->setConnectedNode(upstreamNode);
    if (!nodeOutputString.empty())
    {
        externalOutput->setOutputString(nodeOutputString);
    }
    createdElements.push_back(externalOutput);
    return externalOutput;
}

/// Create an output on the document so that a top-level node can be
/// referenced from within a node graph.  When nodeOutputString is non-empty
/// the top-level node is multi-output and the output selects one of its
/// outputs; otherwise the node has a single implicit output.
OutputPtr getOrCreateDocumentOutput(DocumentPtr doc, NodePtr upstreamNode,
                                    const string& nodeOutputString, vector<ElementPtr>& createdElements)
{
    for (OutputPtr existingOutput : doc->getOutputs())
    {
        if (existingOutput->getConnectedNode() == upstreamNode &&
            existingOutput->getOutputString() == nodeOutputString)
        {
            return existingOutput;
        }
    }

    string outputName = upstreamNode->getName() + "_translation_output";
    if (!nodeOutputString.empty())
    {
        outputName += "_" + nodeOutputString;
    }
    outputName = doc->createValidChildName(outputName);

    OutputPtr externalOutput = doc->addOutput(outputName, upstreamNode->getType());
    externalOutput->setConnectedNode(upstreamNode);
    if (!nodeOutputString.empty())
    {
        externalOutput->setOutputString(nodeOutputString);
    }
    createdElements.push_back(externalOutput);
    return externalOutput;
}

} // anonymous namespace

//
// ShaderTranslator methods
//

void ShaderTranslator::connectTranslationInputs(NodePtr shader, NodeDefPtr translationNodeDef)
{
    vector<InputPtr> origInputs = shader->getInputs();

    for (InputPtr shaderInput : origInputs)
    {
        if (!translationNodeDef->getActiveInput(shaderInput->getName()))
        {
            continue;
        }

        InputPtr input = _translationNode->addInput(shaderInput->getName(), shaderInput->getType());

        // An input may be connected either through an explicit graph or
        // document output (nodegraph/output attributes) or through a direct
        // node reference (nodename/output attributes), including references
        // to a specific output of a multi-output node.  getConnectedNode
        // resolves both styles, while getConnectedOutput only resolves the
        // former, so both must be considered.
        OutputPtr connectedOutput = shaderInput->getConnectedOutput();
        NodePtr connectedNode = shaderInput->getConnectedNode();
        if (connectedNode)
        {
            // For a direct node reference an output string selects a specific
            // output of a multi-output node.  For an explicit graph output the
            // string names the graph output itself and must not be forwarded
            // as a node output selector.
            const bool referencesGraphOutput = (connectedOutput != nullptr);
            string nodeOutputString = referencesGraphOutput ? EMPTY_STRING
                                                            : shaderInput->getOutputString();

            // Nodes with world-space outputs are skipped, with translation
            // being applied to the node directly upstream.  The world-space
            // node is recreated outside of the translation graph when the
            // translated outputs are connected.
            NodePtr worldSpaceNode = connectsToWorldSpaceNode(connectedOutput);
            if (!worldSpaceNode && connectedNode->getCategory() == "normalmap")
            {
                worldSpaceNode = connectedNode;
            }
            if (worldSpaceNode)
            {
                NodePtr upstreamNode = worldSpaceNode->getConnectedNode("in");
                if (upstreamNode)
                {
                    connectedNode = upstreamNode;
                    nodeOutputString = EMPTY_STRING;
                }
                _worldSpaceInputs[shaderInput->getName()] = worldSpaceNode;
            }

            NodeGraphPtr upstreamGraph = getOwningNodeGraph(connectedNode);
            if (upstreamGraph && upstreamGraph != _graph)
            {
                // The upstream node lives in a different node graph, so it
                // must be referenced through an output of that graph.
                OutputPtr externalOutput = getOrCreateGraphOutput(
                    upstreamGraph, connectedNode, nodeOutputString, _createdElements);
                input->setConnectedOutput(externalOutput);
            }
            else if (!upstreamGraph)
            {
                // The upstream node lives at the document's top level, so it
                // must be referenced through a document-level output.
                OutputPtr externalOutput = getOrCreateDocumentOutput(
                    _graph->getDocument(), connectedNode, nodeOutputString, _createdElements);
                input->setConnectedOutput(externalOutput);
            }
            else
            {
                // The upstream node is in the same graph as the translation
                // node.  The output string is retained so that specific
                // outputs of multi-output nodes are preserved.
                input->setConnectedNode(connectedNode);
                if (!nodeOutputString.empty())
                {
                    input->setOutputString(nodeOutputString);
                }
            }
        }
        else if (shaderInput->hasValueString())
        {
            input->setValueString(shaderInput->getValueString());
        }
        else
        {
            throw Exception("Shader input has no associated output or value " + shaderInput->getName());
        }

        string colorSpace = shaderInput->getActiveColorSpace();
        if (!colorSpace.empty())
        {
            input->setColorSpace(colorSpace);
        }
        if (shaderInput->hasUnit())
        {
            input->setUnit(shaderInput->getUnit());
            input->setUnitType(shaderInput->getUnitType());
        }
    }

    // Stash the original shader inputs before removing them, so they can be
    // restored in full on rollback.  All connections are resolved before any
    // input is removed.
    for (InputPtr shaderInput : origInputs)
    {
        ElementPtr inputCopy = stashElement(shaderInput);
        _removedShaderInputs.push_back(
            RemovedShaderInput{ shader, shaderInput->getName(), inputCopy });
    }

    // Determine which outputs of the translation graph were referenced only
    // by the original shader inputs, so they can be removed once no longer
    // needed.  Outputs of other graphs and document-level outputs are never
    // modified.
    std::set<OutputPtr> shaderReferencedOutputs;
    for (InputPtr shaderInput : origInputs)
    {
        if (OutputPtr output = shaderInput->getConnectedOutput())
        {
            if (output->getParent() == _graph)
            {
                shaderReferencedOutputs.insert(output);
            }
        }
    }

    // Remove the original shader inputs.
    for (InputPtr shaderInput : origInputs)
    {
        shader->removeInput(shaderInput->getName());
    }

    // Stash and remove the graph outputs that only the original shader
    // inputs referenced.
    for (OutputPtr output : _graph->getOutputs())
    {
        if (!shaderReferencedOutputs.count(output))
        {
            continue;
        }
        ElementPtr outputCopy = stashElement(output);
        _removedOutputs.push_back(RemovedOutput{ _graph, output->getName(), outputCopy });
        _graph->removeOutput(output->getName());
    }
}

void ShaderTranslator::connectTranslationOutputs(NodePtr shader)
{
    NodeDefPtr nodeDef = _translationNode->getNodeDef();
    if (!nodeDef)
    {
        throw Exception("No nodedef for " + _translationNode->getCategory() + " was found");
    }

    // Iterate through outputs of the translation graph.
    for (OutputPtr translationGraphOutput : nodeDef->getActiveOutputs())
    {
        // Convert output name to input name, using a hardcoded naming convention for now.
        string outputName = translationGraphOutput->getName();
        size_t pos = outputName.find("_out");
        if (pos == string::npos)
        {
            throw Exception("Translation graph output " + outputName + " does not end with '_out'");
        }
        string inputName = outputName.substr(0, pos);

        // Determine the node and output representing this translated stream.
        NodePtr translatedStreamNode = _translationNode;
        string translatedStreamOutput = outputName;

        // Inputs producing world-space coordinates (e.g. "normalmap") are
        // handled outside of the translation graph: the world-space node is
        // recreated in the graph containing the translation node and fed with
        // the tangent-space stream that bypassed the translation math.
        auto worldSpaceIt = _worldSpaceInputs.find(inputName);
        if (worldSpaceIt != _worldSpaceInputs.end())
        {
            NodePtr worldSpaceNode = worldSpaceIt->second;
            InputPtr translationInput = _translationNode->getInput(inputName);
            if (!translationInput)
            {
                continue;
            }

            string worldSpaceName = _graph->createValidChildName(worldSpaceNode->getName());
            NodePtr newWorldSpaceNode = _graph->addNode(
                worldSpaceNode->getCategory(), worldSpaceName, worldSpaceNode->getType());
            _createdElements.push_back(newWorldSpaceNode);

            // Forward any additional inputs of the world-space node, such as
            // the scale and world-space normal/tangent controls of a
            // "normalmap" node.  All attributes, including default geometric
            // properties and color space metadata, are preserved.  Connections
            // to nodes in another graph are routed through that graph's
            // outputs, since bare node names are only valid within one graph.
            NodeDefPtr worldSpaceNodeDef = worldSpaceNode->getNodeDef();
            NodeGraphPtr worldSpaceGraph = getOwningNodeGraph(worldSpaceNode);
            for (InputPtr sourceInput : worldSpaceNode->getInputs())
            {
                if (sourceInput->getName() == "in")
                {
                    continue;
                }
                if (worldSpaceNodeDef && !worldSpaceNodeDef->getActiveInput(sourceInput->getName()))
                {
                    continue;
                }
                InputPtr newInput = newWorldSpaceNode->addInput(sourceInput->getName(), sourceInput->getType());
                NodePtr sourceConnectedNode = sourceInput->getConnectedNode();
                OutputPtr sourceConnectedOutput = sourceInput->getConnectedOutput();
                if (sourceConnectedOutput && (!sourceConnectedNode ||
                    (worldSpaceGraph && worldSpaceGraph != _graph &&
                     getOwningNodeGraph(sourceConnectedNode) == worldSpaceGraph)))
                {
                    // The source input references an output of another graph.
                    NodeGraphPtr sourceGraph = sourceConnectedOutput->getParent()->asA<NodeGraph>();
                    if (sourceGraph && sourceGraph != _graph)
                    {
                        OutputPtr externalOutput = getOrCreateGraphOutput(
                            sourceGraph, sourceConnectedNode, sourceInput->getOutputString(),
                            _createdElements);
                        newInput->setConnectedOutput(externalOutput);
                        continue;
                    }
                    newInput->setConnectedOutput(sourceConnectedOutput);
                }
                else if (sourceConnectedNode && getOwningNodeGraph(sourceConnectedNode) != _graph)
                {
                    // The source input references a node of another graph by name.
                    NodeGraphPtr sourceGraph = getOwningNodeGraph(sourceConnectedNode);
                    if (sourceGraph)
                    {
                        OutputPtr externalOutput = getOrCreateGraphOutput(
                            sourceGraph, sourceConnectedNode, sourceInput->getOutputString(),
                            _createdElements);
                        newInput->setConnectedOutput(externalOutput);
                        continue;
                    }
                    OutputPtr externalOutput = getOrCreateDocumentOutput(
                        _graph->getDocument(), sourceConnectedNode, sourceInput->getOutputString(),
                        _createdElements);
                    newInput->setConnectedOutput(externalOutput);
                }
                else
                {
                    // Preserve value, default geometric property, color space,
                    // unit and all other attributes verbatim.
                    newInput->copyContentFrom(sourceInput);
                }
            }

            // The tangent-space stream that was connected while skipping the
            // original world-space node becomes the input of the new node.
            InputPtr newNodeInput = newWorldSpaceNode->addInput("in", translationInput->getType());
            if (OutputPtr tangentOutput = translationInput->getConnectedOutput())
            {
                newNodeInput->setConnectedOutput(tangentOutput);
            }
            else if (NodePtr tangentNode = translationInput->getConnectedNode())
            {
                newNodeInput->setConnectedNode(tangentNode);
                if (translationInput->hasOutputString())
                {
                    newNodeInput->setOutputString(translationInput->getOutputString());
                }
            }

            translatedStreamNode = newWorldSpaceNode;
            translatedStreamOutput = EMPTY_STRING;
        }

        // Create translated output.
        OutputPtr translatedOutput = _graph->getOutput(outputName);
        if (!translatedOutput)
        {
            translatedOutput = _graph->addOutput(outputName, translationGraphOutput->getType());
            _createdElements.push_back(translatedOutput);
        }
        translatedOutput->setConnectedNode(translatedStreamNode);
        if (!translatedStreamOutput.empty())
        {
            translatedOutput->setOutputString(translatedStreamOutput);
        }

        // Add translated shader input.
        InputPtr translatedShaderInput = shader->getInput(inputName);
        if (!translatedShaderInput)
        {
            translatedShaderInput = shader->addInput(inputName, translationGraphOutput->getType());
            _createdElements.push_back(translatedShaderInput);
        }
        translatedShaderInput->setConnectedOutput(translatedOutput);
    }
}

ElementPtr ShaderTranslator::stashElement(ConstElementPtr element)
{
    if (!_scratchDoc)
    {
        _scratchDoc = createDocument();
    }
    // Removed inputs and outputs may share names, so give every stashed
    // element a unique name within the scratch document.
    string stashName = _scratchDoc->createValidChildName(element->getName());
    ElementPtr copy = _scratchDoc->addChildOfCategory(element->getCategory(), stashName);
    copy->copyContentFrom(element);
    return copy;
}

void ShaderTranslator::rollback()
{
    // Remove created elements in reverse creation order.  Removing a parent
    // element also removes the elements created beneath it.
    for (auto it = _createdElements.rbegin(); it != _createdElements.rend(); ++it)
    {
        ElementPtr element = *it;
        if (element && element->getParent())
        {
            element->getParent()->removeChild(element->getName());
        }
    }
    _createdElements.clear();

    // Restore outputs that were removed from the translation graph.
    for (const RemovedOutput& removed : _removedOutputs)
    {
        if (removed.parent && removed.copy)
        {
            ElementPtr restored = removed.parent->addChildOfCategory(
                removed.copy->getCategory(), removed.originalName);
            restored->copyContentFrom(removed.copy);
        }
    }
    _removedOutputs.clear();

    // Restore the original shader inputs, in their original order.
    for (const RemovedShaderInput& removed : _removedShaderInputs)
    {
        if (removed.shader && removed.copy)
        {
            ElementPtr restored = removed.shader->addChildOfCategory(
                removed.copy->getCategory(), removed.originalName);
            restored->copyContentFrom(removed.copy);
        }
    }
    _removedShaderInputs.clear();
    _scratchDoc.reset();
}

void ShaderTranslator::translateShader(NodePtr shader, const string& destCategory)
{
    _graph = nullptr;
    _translationNode = nullptr;
    _scratchDoc.reset();
    _createdElements.clear();
    _removedOutputs.clear();
    _removedShaderInputs.clear();
    _worldSpaceInputs.clear();

    if (!shader)
    {
        return;
    }

    const string& sourceCategory = shader->getCategory();
    if (sourceCategory == destCategory)
    {
        throw Exception("The source shader \"" + shader->getNamePath() + "\" category is already \"" + destCategory + "\"");
    }

    DocumentPtr doc = shader->getDocument();

    // Choose the graph that will own the translation node.  When all shader
    // inputs reference outputs of a single node graph, that graph is reused
    // so that internal upstream nodes remain in the same scope.  When the
    // inputs are connected from multiple node graphs, or only from top-level
    // nodes, a dedicated translation graph is created and all external
    // connections are made through graph or document outputs.
    std::set<NodeGraphPtr> referencedGraphs;
    for (InputPtr shaderInput : shader->getInputs())
    {
        if (NodePtr upstreamNode = shaderInput->getConnectedNode())
        {
            if (NodeGraphPtr upstreamGraph = getOwningNodeGraph(upstreamNode))
            {
                referencedGraphs.insert(upstreamGraph);
            }
        }
    }

    if (referencedGraphs.size() == 1)
    {
        _graph = *referencedGraphs.begin();
    }
    else
    {
        string graphName = doc->createValidChildName(shader->getName() + "_translation_graph");
        _graph = doc->addNodeGraph(graphName);
        _createdElements.push_back(_graph);
    }

    // Save the original shader identity so it can be restored on rollback.
    _originalCategory = shader->getCategory();
    _originalNodeDefString = shader->getNodeDefString();

    try
    {
        string translateNodeString = sourceCategory + "_to_" + destCategory;
        vector<NodeDefPtr> matchingNodeDefs = doc->getMatchingNodeDefs(translateNodeString);
        if (matchingNodeDefs.empty())
        {
            throw Exception("Shader translation requires a translation nodedef named " + translateNodeString);
        }
        NodeDefPtr translationNodeDef = matchingNodeDefs[0];

        string translationNodeName = _graph->createValidChildName(translateNodeString);
        _translationNode = _graph->addNodeInstance(translationNodeDef, translationNodeName);
        _createdElements.push_back(_translationNode);

        connectTranslationInputs(shader, translationNodeDef);
        shader->setCategory(destCategory);
        shader->removeAttribute(InterfaceElement::NODE_DEF_ATTRIBUTE);
        connectTranslationOutputs(shader);
    }
    catch (const std::exception&)
    {
        // Restore the document to its state before translation so that a
        // failed translation never leaves the shader half-modified.
        rollback();
        if (shader)
        {
            shader->setCategory(_originalCategory);
            if (!_originalNodeDefString.empty())
            {
                shader->setNodeDefString(_originalNodeDefString);
            }
        }
        _graph = nullptr;
        _translationNode = nullptr;
        _worldSpaceInputs.clear();
        throw;
    }

    _graph = nullptr;
    _translationNode = nullptr;
    _scratchDoc.reset();
    _worldSpaceInputs.clear();
}

void ShaderTranslator::translateAllMaterials(DocumentPtr doc, const string& destCategory)
{
    // Snapshot the document before translating any shader, so that it can be
    // fully restored if a later translation fails.  The data library
    // reference is preserved by the deep copy.
    DocumentPtr snapshot = doc->copy();

    // Snapshot the shaders that require translation, since translation
    // modifies the document contents while iterating.
    vector<NodePtr> shaderNodes;
    vector<TypedElementPtr> materialNodes = findRenderableMaterialNodes(doc);
    for (auto elem : materialNodes)
    {
        NodePtr materialNode = elem->asA<Node>();
        if (!materialNode)
        {
            continue;
        }
        for (NodePtr shaderNode : getShaderNodes(materialNode))
        {
            if (shaderNode->getCategory() != destCategory)
            {
                shaderNodes.push_back(shaderNode);
            }
        }
    }

    try
    {
        for (NodePtr shaderNode : shaderNodes)
        {
            translateShader(shaderNode, destCategory);
        }
    }
    catch (const std::exception&)
    {
        // Restore the full document content from the pre-translation
        // snapshot so the original document remains usable.  The data
        // library reference is unchanged.
        doc->clearContent();
        doc->copyContentFrom(snapshot);
        throw;
    }
}

MATERIALX_NAMESPACE_END
