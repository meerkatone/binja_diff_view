#pragma once

#include "binaryninjaapi.h"

// Opens the reference-versus-many setup dialog for currently open BinaryViews.
void openCorpusWorkspace();

// Re-persist corpus pair caches after a BNDB save changes its stable identity.
void persistCorpusMatchesForFile(BinaryNinja::FileMetadata* file);
