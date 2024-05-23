/****************************************************************************
 * Copyright (C) 2012-2016 Woboq GmbH
 * Olivier Goffart <contact at woboq.com>
 * https://woboq.com/codebrowser.html
 *
 * This file is part of the Woboq Code Browser.
 *
 * Commercial License Usage:
 * Licensees holding valid commercial licenses provided by Woboq may use
 * this file in accordance with the terms contained in a written agreement
 * between the licensee and Woboq.
 * For further information see https://woboq.com/codebrowser.html
 *
 * Alternatively, this work may be used under a Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 (CC-BY-NC-SA 3.0) License.
 * http://creativecommons.org/licenses/by-nc-sa/3.0/deed.en_US
 * This license does not allow you to use the code browser to assist the
 * development of your commercial software. If you intent to do so, consider
 * purchasing a commercial licence.
 ****************************************************************************/

#include "projectmanager.h"
#include "filesystem.h"
#include "stringbuilder.h"

#include <clang/Basic/Version.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/fmt/ranges.h"
ProjectManager::ProjectManager(std::string outputPrefix, std::string _dataPath)
    : outputPrefix(outputPrefix)
    , dataPath(std::move(_dataPath))
	,dir_creator_(outputPrefix)
   ,file_index_(outputPrefix + "/fileIndex" + getFileIndexSuffix())
{
    if (dataPath.empty())
        dataPath = "../data";

    for (auto &&info : systemProjects()) {
        addProject(info);
    }
    //createDir();
}

ProjectManager::FileIndex::FileIndex(const std::string &p)
			: path_(p), ofs_(p, std::ios::app) {
				SPDLOG_DEBUG("Construct of file index with path: {}", p);
				assert(ofs_.is_open());
			}

bool ProjectManager::addProject(ProjectInfo info)
{
    if (info.source_path.empty())
        return false;
    llvm::SmallString<256> filename;
    canonicalize(info.source_path, filename);
    SPDLOG_DEBUG("Add project: {}", filename.c_str());
    if (filename.empty())
        return false;
    if (filename[filename.size() - 1] != '/')
        filename += '/';
    info.source_path = filename.c_str();

    projects.push_back(std::move(info));
    return true;
}

ProjectInfo *ProjectManager::projectForFile(llvm::StringRef filename)
{
    unsigned int match_length = 0;
    ProjectInfo *result = nullptr;

    for (auto &it : projects) {
        const std::string &source_path = it.source_path;
        if (source_path.size() < match_length) {
            continue;
        }
        if (filename.startswith(source_path)) {
            result = &it;
            match_length = source_path.size();
        }
    }
    return result;
}


//TODO find the corresponding entry for create file
bool ProjectManager::shouldProcess0(llvm::StringRef filename, ProjectInfo *project)
{
    if (!project)
    {
    	SPDLOG_DEBUG("should not process: {}", filename.str());
        return false;
    }
    if (project->type == ProjectInfo::External)
    {
    	SPDLOG_DEBUG("should not process since it's external: {}", filename.str());
        return false;
    }
    return true;
}


//TODO find the corresponding entry for create file
bool ProjectManager::shouldProcess(llvm::StringRef filename, ProjectInfo *project)
{
    if (!project)
    {
    	SPDLOG_DEBUG("should not process: {}", filename.str());
        return false;
    }
    if (project->type == ProjectInfo::External)
    {
    	SPDLOG_DEBUG("should not process since it's external: {}", filename.str());
        return false;
    }

    std::string fn = outputPrefix % "/" % project->name % "/"
        % filename.substr(project->source_path.size()) % ".html";
    auto has = addFile_Locked(fn);
    SPDLOG_DEBUG("The final file name: {}, add lock succeed:{}", fn, has);
    return has;
    //return !llvm::sys::fs::exists(fn);
    // || boost::filesystem::last_write_time(p) < entry->getModificationTime();
}


void ProjectManager::createDir()
{
	assert(false);
}
ProjectManager::DirCreator::DirCreator(const std::string& outputPrefix) {
	SPDLOG_DEBUG("Create dir for prefix begin:{}", outputPrefix);
	auto e = create_directories(outputPrefix);
	assert(!e);
    e = create_directories(llvm::Twine(outputPrefix, "/refs/_M"));
	assert(!e);
    e = create_directories(llvm::Twine(outputPrefix, "/fnSearch"));
	assert(!e);
	SPDLOG_DEBUG("Create dir for prefix done:{}", outputPrefix);
}

ProjectManager::RefFile::RefFile(const std::string &p)
	: path_(p), ofs_(p, error_code, llvm::sys::fs::OF_Append) {
				assert(!ofs_.has_error());
			}
