/*
 * The Sleuth Kit
 *
 * Contact: Brian Carrier [carrier <at> sleuthkit [dot] org]
 * Copyright (c) 2010-2012 Basis Technology Corporation. All Rights
 * reserved.
 *
 * This software is distributed under the Common Public License 1.0
 */

/** \file InterestingFiles.cpp
 * This file contains the implementation of a module that saves interesting 
 * files recorded on the blackboard to a user-specified output directory.
 */

// System includes
#include <string>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <iostream>

// Framework includes
#include "TskModuleDev.h"

// Poco includes
#include "Poco/Path.h"
#include "Poco/File.h"
#include "Poco/FileStream.h"
#include "Poco/Exception.h"
#include "Poco/XML/XMLWriter.h"
#include "Poco/DOM/AutoPtr.h"
#include "Poco/DOM/Document.h"
#include "Poco/DOM/Element.h"
#include "Poco/DOM/Attr.h"
#include "Poco/DOM/DOMWriter.h"
#include "Poco/DOM/Text.h"
#include "Poco/DOM/Text.h"
#include "Poco/DOM/DOMException.h"

namespace
{
    // The interesting files will be saved to this location. The path is passed to
    // the module as an argument to the initialize() module API and cached here 
    // for use in the report() module API.
    std::string outputFolderPath;

    typedef std::map<std::string, std::string> FileSets; 
    typedef std::multimap<std::string, TskBlackboardArtifact> FileSetHits;
    typedef std::pair<FileSetHits::iterator, FileSetHits::iterator> FileSetHitsRange; 

    // Helper function to create a directory. Throws a TskException capturing the module name on failure.
    void createDirectory(const std::string &path)
    {
        try
        {
            Poco::File dir(path);
            dir.createDirectories();
        } 
        catch (Poco::Exception &ex) 
        {
            std::stringstream msg;
            msg << L"SaveInterestingFilesModule failed to create directory '" << outputFolderPath << "' : " << ex.message();
            throw TskException(msg.str());
        }
    }

    // Helper function to add a saved file or directory to a saved files report. 
    void addFileToReport(const TskFile &file, const std::string &filePath, Poco::XML::Document *report)
    {
        Poco::XML::Element *reportRoot = static_cast<Poco::XML::Element*>(report->firstChild());

        Poco::AutoPtr<Poco::XML::Element> fileElement; 
        if (file.getMetaType() == TSK_FS_META_TYPE_DIR)
        {
            fileElement = report->createElement("SavedDirectory");
        }
        else
        {
            fileElement = report->createElement("SavedFile");
        }
        reportRoot->appendChild(fileElement);

        Poco::AutoPtr<Poco::XML::Element> savedPathElement = report->createElement("Path");
        fileElement->appendChild(savedPathElement);        
        Poco::AutoPtr<Poco::XML::Text> savedPathText = report->createTextNode(filePath);
        savedPathElement->appendChild(savedPathText);

        Poco::AutoPtr<Poco::XML::Element> originalPathElement = report->createElement("OriginalPath");        
        fileElement->appendChild(originalPathElement);
        Poco::AutoPtr<Poco::XML::Text> originalPathText = report->createTextNode(file.getUniquePath());
        originalPathElement->appendChild(originalPathText);

        if (file.getMetaType() != TSK_FS_META_TYPE_DIR)
        {
            // This element will be empty unless a hash calculation module has operated on the file.
            Poco::AutoPtr<Poco::XML::Element> md5HashElement = report->createElement("MD5");        
            fileElement->appendChild(md5HashElement);                
            Poco::AutoPtr<Poco::XML::Text> md5HashText = report->createTextNode(file.getHash(TskImgDB::MD5));
            md5HashElement->appendChild(md5HashText);
        }
    }

    // Helper function to recursively write out the contents of a directory. Throws TskException on failure.
    void saveDirectoryContents(const std::string &dirPath, const TskFile &dir, Poco::XML::Document *report)
    {
        // Construct a query for the file records corresponding to the files in the directory and fetch them.
        std::stringstream condition; 
        condition << "WHERE par_file_id = " << dir.getId();
        std::vector<const TskFileRecord> fileRecs = TskServices::Instance().getImgDB().getFileRecords(condition.str());

        // Save each file and subdirectory in the directory.
        for (std::vector<const TskFileRecord>::const_iterator fileRec = fileRecs.begin(); fileRec != fileRecs.end(); ++fileRec)
        {
            std::auto_ptr<TskFile> file(TskServices::Instance().getFileManager().getFile((*fileRec).fileId));

            if (file->getMetaType() == TSK_FS_META_TYPE_DIR)
            {
                // Create a subdirectory to hold the contents of this subdirectory.
                std::stringstream subDirPath;
                subDirPath << dirPath << Poco::Path::separator() << file->getName();
                createDirectory(subDirPath.str());

                // Recurse into the subdirectory.
                saveDirectoryContents(subDirPath.str(), *file, report);
            }
            else
            {
                // Save the file.
                std::stringstream filePath;
                filePath << dirPath << Poco::Path::separator() << file->getName();
                TskServices::Instance().getFileManager().copyFile(file.get(), TskUtilities::toUTF16(filePath.str()));
                addFileToReport(*file, filePath.str(), report);
            }
        }
    }

    // Helper function to save the contents of an interesting directory to the output folder. Throws TskException on failure.
    void saveInterestingDirectory(const TskFile &dir, const std::string &fileSetFolderPath, Poco::XML::Document *report)
    {
        // Make a subdirectory of the output folder named for the interesting file search set and create a further subdirectory
        // corresponding to the directory to be saved. The resulting directory structure will look like this:
        // <output folder>/
        //      <interesting file set name>/
        //          <directory name>_<file id>/ /*Suffix the directory with its its file id to ensure uniqueness*/
        //              <directory name>/
        //                  <contents of directory including subdirectories>
        //
        std::stringstream path;
        path << fileSetFolderPath << Poco::Path::separator() << dir.getName() << '_' << dir.getId() << Poco::Path::separator() << dir.getName();
        createDirectory(path.str());

        addFileToReport(dir, path.str(), report);

        saveDirectoryContents(path.str(), dir, report);
    }

    // Helper function to save the contents of an interesting file to the output folder. Throws TskException on failure.
    void saveInterestingFile(const TskFile &file, const std::string &fileSetFolderPath, Poco::XML::Document *report)
    {
        // Construct a path to write the contents of the file to a subdirectory of the output folder named for the interesting file search
        // set. The resulting directory structure will look like this:
        // <output folder>/
        //      <interesting file set name>/
        //          <file name>_<fileId>.<ext> /*Suffix the file with its its file id to ensure uniqueness*/
        std::string fileName = file.getName();
        std::stringstream id;
        id << '_' << file.getId();
        std::string::size_type pos = 0;
        if ((pos = fileName.rfind(".")) != std::string::npos && pos != 0)
        {
            // The file name has a conventional extension. Insert the file id before the '.' of the extension.
            fileName.insert(pos, id.str());
        }
        else
        {
            // The file has no extension or the only '.' in the file is an initial '.', as in a hidden file.
            // Add the file id to the end of the file name.
            fileName.append(id.str());
        }
        std::stringstream filePath;
        filePath << fileSetFolderPath.c_str() << Poco::Path::separator() << fileName.c_str();
    
        // Save the file.
        TskServices::Instance().getFileManager().copyFile(file.getId(), TskUtilities::toUTF16(filePath.str()));

        addFileToReport(file, filePath.str(), report);
    }

    // Helper function to save the files corresponding to the file set hit artifacts for a specified interesting files set.
    void saveFiles(const std::string &setName, const std::string &setDescription, FileSetHitsRange fileSetHitsRange, TskModule::Status &returnCode)
    {
        // Start an XML report of the files in the set.
        Poco::AutoPtr<Poco::XML::Document> report = new Poco::XML::Document();
        Poco::AutoPtr<Poco::XML::Element> reportRoot = report->createElement("InterestingFileSet");
        reportRoot->setAttribute("name", setName);
        reportRoot->setAttribute("description", setDescription);
        report->appendChild(reportRoot);

        // Make a subdirectory of the output folder named for the interesting file set.
        std::stringstream fileSetFolderPath;
        fileSetFolderPath << outputFolderPath << Poco::Path::separator() << setName;
        createDirectory(fileSetFolderPath.str());
        
        // Save all of the files in the set.
        for (FileSetHits::iterator fileHit = fileSetHitsRange.first; fileHit != fileSetHitsRange.second; ++fileHit)
        {
            try
            {
                std::auto_ptr<TskFile> file(TskServices::Instance().getFileManager().getFile((*fileHit).second.getObjectID()));
                if (file->getMetaType() == TSK_FS_META_TYPE_DIR)
                {
                     saveInterestingDirectory(*file, fileSetFolderPath.str(), report); 
                }
                else
                {
                    saveInterestingFile(*file, fileSetFolderPath.str(), report);
                }
            }
            catch(TskException &ex)
            {
                // Log the error and try the next file hit, but signal that an error occurred with a FAIL return code.
                LOGERROR(TskUtilities::toUTF16(ex.message()));
                returnCode = TskModule::FAIL;
            }
        }

        // Write out the completed XML report.
        fileSetFolderPath << Poco::Path::separator() << setName << ".xml";
        Poco::FileStream reportFile(fileSetFolderPath.str());
        Poco::XML::DOMWriter writer;
        writer.setNewLine("\n");
        writer.setOptions(Poco::XML::XMLWriter::PRETTY_PRINT);
        writer.writeNode(reportFile, report);
    }
}

extern "C" 
{
    /**
     * Module identification function. 
     *
     * @return The name of the module.
     */
    TSK_MODULE_EXPORT const char *name()
    {
        return "SaveInterestingFiles";
    }

    /**
     * Module identification function. 
     *
     * @return A description of the module.
     */
    TSK_MODULE_EXPORT const char *description()
    {
        return "Saves files and directories that were flagged as being interesting to a location for further analysis";
    }

    /**
     * Module identification function. 
     *
     * @return The version of the module.
     */
    TSK_MODULE_EXPORT const char *version()
    {
        return "0.0.0";
    }

    /**
     * Module initialization function. Receives an output folder path as the location
     * for saving the files corresponding to interesting file set hits.
     *
     * @param args Output folder path.
     * @return TskModule::OK 
     */
    TSK_MODULE_EXPORT TskModule::Status initialize(const char* arguments)
    {
        // Reset the output folder path in case initialize() is called more than once.
        outputFolderPath.clear();

        if (arguments != NULL)
        {
            outputFolderPath = arguments;
        }

        if (outputFolderPath.empty())
        {
            std::stringstream pathBuilder;
            pathBuilder << "#OUT_DIR#" << Poco::Path::separator() << "InterestingFiles";
            outputFolderPath = ExpandSystemPropertyMacros(pathBuilder.str());
        }

        return TskModule::OK;
    }

    /**
     * Module execution function. saves interesting files recorded on the 
     * blackboard to a user-specified output directory.
     *
     * @returns TskModule::OK on success if all files saved, TskModule::FAIL if one or more files were not saved
     */
    TSK_MODULE_EXPORT TskModule::Status report()
    {
        TskModule::Status returnCode = TskModule::OK;
        
        LOGINFO(L"SaveInterestingFilesModule save operations started");

        try
        {
            // Make the output directory specified using the initialize() API.
            createDirectory(outputFolderPath);

            // Get the interesting file set hits from the blackboard and sort them by set name.
            FileSets fileSets;
            FileSetHits fileSetHits;
            std::vector<TskBlackboardArtifact> fileSetHitArtifacts = TskServices::Instance().getBlackboard().getArtifacts(TSK_INTERESTING_FILE_HIT);
            for (std::vector<TskBlackboardArtifact>::iterator fileHit = fileSetHitArtifacts.begin(); fileHit != fileSetHitArtifacts.end(); ++fileHit)
            {
                // Find the set name attrbute of the artifact.
                bool setNameFound = false;
                std::vector<TskBlackboardAttribute> attrs = (*fileHit).getAttributes();
                for (std::vector<TskBlackboardAttribute>::iterator attr = attrs.begin(); attr != attrs.end(); ++attr)
                {
                    if ((*attr).getAttributeTypeID() == TSK_SET_NAME)
                    {
                        setNameFound = true;
                        
                        // Save the set name and description, using a map to ensure that these values are saved once per file set.
                        fileSets.insert(make_pair((*attr).getValueString(), (*attr).getContext()));
                        
                        // Drop the artifact into a multimap to allow for retrieval of all of the file hits for a file set as an 
                        // iterator range.
                        fileSetHits.insert(make_pair((*attr).getValueString(), (*fileHit)));
                    }
                }

                if (!setNameFound)
                {
                    // Log the error and try the next artifact.
                    std::wstringstream msg;
                    msg << L"SaveInterestingFilesModule failed to find set name TSK_SET_NAME for TSK_INTERESTING_FILE_HIT artifact with id " << (*fileHit).getArtifactID();
                    LOGERROR(msg.str());
                }
            }

            // Save the interesting files to the output directory, file set by file set.
            for (map<std::string, std::string>::const_iterator fileSet = fileSets.begin(); fileSet != fileSets.end(); ++fileSet)
            {
                // Get the file hits for the file set as an iterator range.
                FileSetHitsRange fileSetHitsRange = fileSetHits.equal_range((*fileSet).first); 

                // Save the files corresponding to the file hit artifacts.
                saveFiles((*fileSet).first, (*fileSet).second, fileSetHitsRange, returnCode);
            }
        }
        catch(TskException &ex)
        {
            LOGERROR(TskUtilities::toUTF16(ex.message()));
            returnCode = TskModule::FAIL;
        }

        LOGINFO(L"SaveInterestingFilesModule save operations finished");
        
        return returnCode;
    }

    /**
     * Module cleanup function. This imodule does not need to free any resources 
     * allocated during initialization or execution.
     *
     * @returns TskModule::OK
     */
    TSK_MODULE_EXPORT TskModule::Status finalize()
    {
        return TskModule::OK;
    }
}
