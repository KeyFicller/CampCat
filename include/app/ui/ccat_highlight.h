#pragma once

#include "TextEditor.h" // the editor fetched by CMake; its include dir is PUBLIC

namespace campcat::app {

/// The `.ccat` syntax definition for the script editor.
const TextEditor::LanguageDefinition &ccat_language();

/// The palette the definition above is drawn with, matching the app's dark theme.
const TextEditor::Palette &ccat_palette();

} // namespace campcat::app
