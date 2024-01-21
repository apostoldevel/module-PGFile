/*++

Program name:

  Apostol CRM

Module Name:

  PGFile.hpp

Notices:

  Module: PG File

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#ifndef APOSTOL_PG_FILE_HPP
#define APOSTOL_PG_FILE_HPP
//----------------------------------------------------------------------------------------------------------------------

#include "FileCommon.hpp"

extern "C++" {

namespace Apostol {

    namespace Module {

        //--------------------------------------------------------------------------------------------------------------

        //-- CPGFile ---------------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        class CPGFile: public CFileCommon {
        private:

            CDateTime m_CheckDate;

            void InitListen();
            void CheckListen();

        protected:

            void DoFile(CQueueHandler *AHandler);

            void DoPostgresNotify(CPQConnection *AConnection, PGnotify *ANotify) override;

        public:

            explicit CPGFile(CModuleProcess *AProcess);

            ~CPGFile() override = default;

            static class CPGFile *CreateModule(CModuleProcess *AProcess) {
                return new CPGFile(AProcess);
            }

            void Heartbeat(CDateTime Now) override;

            bool Enabled() override;

        };
    }
}

using namespace Apostol::Module;
}
#endif //APOSTOL_PG_FILE_HPP
