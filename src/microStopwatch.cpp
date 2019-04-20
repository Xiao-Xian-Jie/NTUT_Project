#include <microStopwatch.hpp>
namespace myClass
{
    MicroStopwatch::MicroStopwatch() : elapsedCurrent(0), elapsedTotal(0), name("MicroStopwatch" + std::to_string(microStopwatchCount++)) { }
    MicroStopwatch::MicroStopwatch(std::string name) : elapsedCurrent(0), elapsedTotal(0), name(name) { }

    void MicroStopwatch::rename(const std::string &name) { 
        this->name = name; 
    }

    void MicroStopwatch::tic(bool ticPrintName)
    {
        tictic = boost::posix_time::microsec_clock::local_time ();
        if(ticPrintName) {
            std::cout << this->name << "... ";
        }
        this->ticPrintName = ticPrintName;
    }

    std::string MicroStopwatch::toc_string() {
        toctoc = boost::posix_time::microsec_clock::local_time ();
        elapsedCurrent = (toctoc - tictic).total_microseconds();
        elapsedTotal += elapsedCurrent;
        return myFunction::commaFix(elapsedCurrent) + " us";
    }

    void MicroStopwatch::toc()
    {
        if(!ticPrintName) {
            std::cout << this->name << ": ";
        }
        std::cout << this->toc_string() << std::endl;
    }

    std::string MicroStopwatch::elapsed_string() {
        return myFunction::commaFix(elapsedTotal) + " us";
    }

    void MicroStopwatch::elapsed() {
        if(!ticPrintName) {
            std::cout << this->name << ": ";
        }
        std::cout << this->elapsed_string() << std::endl;
    }
}