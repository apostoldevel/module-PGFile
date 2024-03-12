/*++

Program name:

  Apostol CRM

Module Name:

  PGFile.cpp

Notices:

  Module: PGFile

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

//----------------------------------------------------------------------------------------------------------------------

#include "Core.hpp"
#include "PGFile.hpp"
//----------------------------------------------------------------------------------------------------------------------

#define PG_CONFIG_NAME "helper"
#define PG_LISTEN_NAME "file"

#define QUERY_INDEX_AUTH     0
#define QUERY_INDEX_DATA     1
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace Module {

        //--------------------------------------------------------------------------------------------------------------

        //-- CPGFile ---------------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CPGFile::CPGFile(CModuleProcess *AProcess): CFileCommon(AProcess, "pg file", "module/PGFile") {
            m_CheckDate = 0;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CPGFile::DoFile(CQueueHandler *AHandler) {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                auto pHandler = dynamic_cast<CFileHandler *> (APollQuery->Binding());

                if (pHandler == nullptr)
                    return;

                CPQueryResults pqResults;

                try {
                    CApostolModule::QueryToResults(APollQuery, pqResults);

                    const auto &authorize = pqResults[QUERY_INDEX_AUTH].First();

                    if (authorize["authorized"] != "t")
                        throw Delphi::Exception::ExceptionFrm("Authorization failed: %s", authorize["message"].c_str());

                    if (pqResults[QUERY_INDEX_DATA].Count() == 0) {
                        DeleteHandler(pHandler);
                        return;
                    }

                    const auto &caFile = pqResults[QUERY_INDEX_DATA].First();

                    const auto &operation = pHandler->Operation();
                    const auto &old_hash = pHandler->Hash();
                    const CString oldAbsoluteName(pHandler->AbsoluteName());

                    const auto &type = caFile["type"];
                    const auto &path = caFile["path"];
                    const auto &name = caFile["name"];
                    const auto &hash = caFile["hash"];
                    const auto &mime = caFile["mime"];
                    const auto &data = caFile["data"];
                    const auto &done = caFile["done"];
                    const auto &fail = caFile["fail"];

                    const auto &caPath = m_Path + (path_separator(path.front()) ? path.substr(1) : path);
                    ForceDirectories(caPath.c_str(), 0755);
                    const auto &caAbsoluteName = path_separator(caPath.back()) ? caPath + name : caPath + "/" + name;

                    if (data.empty()) {
                        DeleteHandler(pHandler);
                        return;
                    }

                    pHandler->AbsoluteName() = caAbsoluteName;
                    pHandler->Done() = done;

                    if (type == "-") {
                        const bool changed = (operation == "UPDATE" && ((oldAbsoluteName != caAbsoluteName) || (old_hash != hash)));

                        if (operation == "UPDATE" && (oldAbsoluteName != caAbsoluteName)) {
                            DeleteFile(oldAbsoluteName);
                        }

                        if (operation == "INSERT" || changed) {
                            CHTTPReply Reply;

                            DeleteFile(caAbsoluteName);

                            Reply.Content = base64_decode(squeeze(data));
                            Reply.ContentLength = Reply.Content.Length();
                            Reply.Content.SaveToFile(caAbsoluteName.c_str());
                            Reply.Headers.Values("Content-Type", mime);

                            DoDone(pHandler, Reply);

                            return;
                        }
                    } else if (type == "l") {
                        const auto &decode = base64_decode(squeeze(data));

                        if ((decode.substr(0, 8) == FILE_COMMON_HTTPS || decode.substr(0, 7) == FILE_COMMON_HTTP)) {
                            pHandler->URI() = decode;
                            pHandler->Done() = done;
                            pHandler->Fail() = fail;

                            if (m_Type == "curl") {
                                DoCURL(pHandler);
                            } else {
                                DoFetch(pHandler);
                            }

                            return;
                        }
                    } else if (type == "s") {
                        DeleteFile(oldAbsoluteName);
                    }
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }

                DeleteHandler(pHandler);
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoError(E);
                auto pHandler = dynamic_cast<CFileHandler *> (APollQuery->Binding());
                if (pHandler != nullptr) {
                    DeleteHandler(pHandler);
                }
            };

            auto pHandler = dynamic_cast<CFileHandler *> (AHandler);

            AHandler->Allow(false);
            IncProgress();

            const auto &operation = pHandler->Operation();
            const auto &path = pHandler->Path();
            const auto &name = pHandler->Name();

            const auto &caPath = m_Path + (path_separator(path.front()) ? path.substr(1) : path);
            const auto &caAbsoluteName = path_separator(caPath.back()) ? caPath + name : caPath + "/" + name;

            pHandler->AbsoluteName() = caAbsoluteName;

            if (operation == "DELETE") {
                DeleteFile(caAbsoluteName);
                DeleteHandler(AHandler);
            } else {
                CStringList SQL;

                api::authorize(SQL, m_Session);
                api::get_file(SQL, pHandler->FileId());

                try {
                    ExecSQL(SQL, AHandler, OnExecuted, OnException);
                } catch (Delphi::Exception::Exception &E) {
                    DoError(AHandler, E.Message());
                }
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CPGFile::DoPostgresNotify(CPQConnection *AConnection, PGnotify *ANotify) {
            DebugNotify(AConnection, ANotify);

            if (CompareString(ANotify->relname, PG_LISTEN_NAME) == 0) {
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                new CFileHandler(this, ANotify->extra, [this](auto &&Handler) { DoFile(Handler); });
#else
                new CFileHandler(this, ANotify->extra, std::bind(&CPGFile::DoFile, this, _1));
#endif
                UnloadQueue();
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CPGFile::InitListen() {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {
                try {
                    auto pResult = APollQuery->Results(0);

                    if (pResult->ExecStatus() != PGRES_COMMAND_OK) {
                        throw Delphi::Exception::EDBError(pResult->GetErrorMessage());
                    }

                    APollQuery->Connection()->Listeners().Add(PG_LISTEN_NAME);
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                    APollQuery->Connection()->OnNotify([this](auto && APollQuery, auto && ANotify) { DoPostgresNotify(APollQuery, ANotify); });
#else
                    APollQuery->Connection()->OnNotify(std::bind(&CPGFile::DoPostgresNotify, this, _1, _2));
#endif
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoError(E);
            };

            CStringList SQL;

            SQL.Add("LISTEN " PG_LISTEN_NAME ";");

            try {
                ExecSQL(SQL, nullptr, OnExecuted, OnException, PG_CONFIG_NAME);
            } catch (Delphi::Exception::Exception &E) {
                DoError(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CPGFile::CheckListen() {
            if (!PQClient(PG_CONFIG_NAME).CheckListen(PG_LISTEN_NAME))
                InitListen();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CPGFile::Heartbeat(CDateTime Now) {
            if ((Now >= m_CheckDate)) {
                m_CheckDate = Now + (CDateTime) 30 / SecsPerDay; // 30 sec
                CheckListen();
            }

            if ((Now >= m_AuthDate)) {
                m_AuthDate = Now + (CDateTime) 5 / SecsPerDay; // 5 sec
                Authentication();
            }

            UnloadQueue();
            CheckTimeOut(Now);
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CPGFile::Enabled() {
            if (m_ModuleStatus == msUnknown)
                m_ModuleStatus = Config()->IniFile().ReadBool(SectionName().c_str(), "enable", true) ? msEnabled : msDisabled;
            return m_ModuleStatus == msEnabled;
        }
        //--------------------------------------------------------------------------------------------------------------
    }
}
}