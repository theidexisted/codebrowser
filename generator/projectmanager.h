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

#pragma once

#include <llvm/ADT/StringRef.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <fstream>

#include "logger.h"

struct ProjectInfo
{
    std::string name;
    std::string source_path;
    //    std::string description;
    //    std::string version_info;
    //    std::string repo_url; //may contains tags;
    std::string revision;

    std::string external_root_url;

    // TODO
    std::string fileRepoUrl(const std::string &file) const
    {
        return {};
    }
    enum Type {
        Normal,
        Internal, // includes and stuffs
        External, // links to external projects somewhere else, do not generate refs or anything,
                  //  and link to a different ref source
    } type = Normal;

    ProjectInfo(std::string name, std::string source_path, Type t = Normal)
        : name(std::move(name))
        , source_path(std::move(source_path))
        , type(t)
    {
    }
    ProjectInfo(std::string name, std::string source_path, std::string rev)
        : name(std::move(name))
        , source_path(std::move(source_path))
        , revision(std::move(rev))
    {
    }
};
#include <ostream>
#include <llvm/Support/raw_ostream.h>

struct ProjectManager
{
    explicit ProjectManager(std::string outputPrefix, std::string _dataPath);

    bool addProject(ProjectInfo info);

    void createDir();

    std::vector<ProjectInfo> projects;

    std::string outputPrefix;
    std::string dataPath;

    // the file name need to be canonicalized
    ProjectInfo *projectForFile(llvm::StringRef filename); // don't keep a cache

    // return true if the filename should be proesseded.
    // 'project' is the value returned by projectForFile
    bool shouldProcess(llvm::StringRef filename, ProjectInfo *project);
    bool shouldProcess0(llvm::StringRef filename, ProjectInfo *project);

    //std::string includeRecovery(llvm::StringRef includeName, llvm::StringRef from);
    //
    struct DirCreator {
		DirCreator(const std::string& d);
    };
    class FileIndex {
    	public:
		FileIndex(const std::string &p);
		FileIndex(const FileIndex&) = delete;
		//FileIndex(FileIndex&&) = default;
		void AppendLine_Locked(const std::string& s) {
			std::lock_guard lg(mutex_);
			ofs_<<s;
		}
		private:
		std::mutex mutex_;
		std::string path_;
		std::ofstream ofs_;
    };
    class RefFile {
    	public:
		RefFile(const std::string &p);
		RefFile(const FileIndex&) = delete;
		void AppendLine_Locked(const std::string& s) {
			std::lock_guard lg(mutex_);
			ofs_<<s;
		}
		private:
		std::mutex mutex_;
		static inline thread_local std::error_code error_code;
		std::string path_;
		//std::ofstream ofs_;
		llvm::raw_fd_ostream ofs_;
    };

    RefFile& GetRefFile(const std::string& s) {
		std::lock_guard lg(mutex_);
		auto [itr, _] = ref_files.try_emplace(s, s);
		return itr->second;
    }
    RefFile& GetFuncIndexFile(const std::string& s) {
		std::lock_guard lg(mutex_);
		auto [itr, _] = func_index_files.try_emplace(s, s);
		return itr->second;
    }
	void AddFileIndex(const std::string &s) {
		file_index_.AppendLine_Locked(s);
	}

private:
    static std::vector<ProjectInfo> systemProjects();
    /*
    bool hasFile_Locked(const std::string& file) {
		std::lock_guard lg(mutex_);
		return exists_files_.count(file);
    }
    */
    bool addFile_Locked(const std::string& file) {
		std::lock_guard lg(mutex_);
		auto [_, suc] = exists_files_.insert(file);
		return suc;
    }
	DirCreator dir_creator_;
	std::mutex mutex_;
	std::unordered_set<std::string> exists_files_;
    std::unordered_multimap<std::string, std::string> includeRecoveryCache;

    FileIndex file_index_;
	std::unordered_map<std::string, RefFile> ref_files;
	std::unordered_map<std::string, RefFile> func_index_files;

};
